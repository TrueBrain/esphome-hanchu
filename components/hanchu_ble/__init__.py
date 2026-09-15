import re
from typing import NamedTuple

from esphome import automation
import esphome.codegen as cg
from esphome.components import ble_client
import esphome.config_validation as cv
from esphome.const import (
    CONF_GROUP,
    CONF_ID,
    CONF_KEY,
    CONF_PLATFORM,
    CONF_TRIGGER_ID,
    CONF_UPDATE_INTERVAL,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_POWER_FACTOR,
    DEVICE_CLASS_REACTIVE_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_HERTZ,
    UNIT_KILOWATT_HOURS,
    UNIT_MINUTE,
    UNIT_PERCENT,
    UNIT_VOLT,
    UNIT_VOLT_AMPS_REACTIVE,
    UNIT_WATT,
)
import esphome.final_validate as fv

CODEOWNERS = ["@TrueBrain"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["json", "sensor", "text_sensor"]
MULTI_CONF = True

CONF_HANCHU_BLE_ID = "hanchu_ble_id"
CONF_MAX_KEYS_PER_REQUEST = "max_keys_per_request"
CONF_REQUEST_TIMEOUT = "request_timeout"
CONF_ON_POLL = "on_poll"
CONF_CUSTOM = "custom"

hanchu_ble_ns = cg.esphome_ns.namespace("hanchu_ble")
HanchuBle = hanchu_ble_ns.class_(
    "HanchuBle", cg.PollingComponent, ble_client.BLEClientNode
)
HanchuBlePollTrigger = hanchu_ble_ns.class_(
    "HanchuBlePollTrigger", automation.Trigger.template()
)
ONCE_AFTER_BOOT = cg.RawExpression("esphome::hanchu_ble::ONCE_AFTER_BOOT")

# Known keys, by what they describe. Keys of the same group are requested together by
# default, so related values come from a single reply. Source:
# https://github.com/upton68/hanchu-ess-ble/blob/main/docs/hanchu-ble-local-protocol-mapping.md
KEY_GROUPS = {
    "device": ["P002", "P003", "P008", "P005", "P006", "P007", "P139", "L023", "P000", "L034", "P011"],
    "pv": ["P024", "P025", "P026", "P027", "P028", "P029", "P060", "P061", "P062", "P237", "P242", "P243", "P244"],
    "grid": ["P644", "P044", "P045", "P053", "P055", "P056", "P057", "P640", "P641", "P642", "P643"],
    "battery": ["P071", "P069", "P067", "P068", "P070", "P075", "P076", "P088", "P142", "P063", "P064"],
    "eps": ["P079", "P080", "P081", "P082", "P083", "P084", "P085"],
    "settings": ["P651", "L017", "L018", "P647", "P648", "P772", "L074", "P236", "P245"],
    "slots": ["L005", "L006", "L007", "L008", "L009", "L010", "L011", "L012", "L013", "L014", "L015", "L016"],
    "clock": ["L094", "L020", "L096"],
    "unmapped": ["P498", "P499", "P240", "P241"],
}
KEY_TO_GROUP = {key: group for group, keys in KEY_GROUPS.items() for key in keys}


class SensorSpec(NamedTuple):
    key: str
    unit: str | None = None
    device_class: str | None = None
    state_class: str | None = None
    accuracy_decimals: int = 0
    multiplier: float = 1.0


def _power(key):
    return SensorSpec(key, UNIT_WATT, DEVICE_CLASS_POWER, STATE_CLASS_MEASUREMENT)


def _limit(key):
    return SensorSpec(key, UNIT_WATT, DEVICE_CLASS_POWER)


def _reactive_power(key):
    return SensorSpec(key, UNIT_VOLT_AMPS_REACTIVE, DEVICE_CLASS_REACTIVE_POWER, STATE_CLASS_MEASUREMENT)


def _energy(key):
    return SensorSpec(key, UNIT_KILOWATT_HOURS, DEVICE_CLASS_ENERGY, STATE_CLASS_TOTAL_INCREASING, 1)


def _energy_snapshot(key):
    # No state class: the value jumps at midnight instead of counting up
    return SensorSpec(key, UNIT_KILOWATT_HOURS, DEVICE_CLASS_ENERGY, None, 1)


def _voltage(key):
    return SensorSpec(key, UNIT_VOLT, DEVICE_CLASS_VOLTAGE, STATE_CLASS_MEASUREMENT, 1)


def _current(key, decimals=1):
    return SensorSpec(key, UNIT_AMPERE, DEVICE_CLASS_CURRENT, STATE_CLASS_MEASUREMENT, decimals)


def _temperature(key):
    return SensorSpec(key, UNIT_CELSIUS, DEVICE_CLASS_TEMPERATURE, STATE_CLASS_MEASUREMENT, 1)


def _frequency(key):
    return SensorSpec(key, UNIT_HERTZ, DEVICE_CLASS_FREQUENCY, STATE_CLASS_MEASUREMENT, 2)


def _soc(key, multiplier):
    return SensorSpec(key, UNIT_PERCENT, DEVICE_CLASS_BATTERY, STATE_CLASS_MEASUREMENT, 0, multiplier)


def _percent(key):
    return SensorSpec(key, UNIT_PERCENT)


# Named sensors. Values arrive in the unit the name promises; signs are as the device sends them.
SENSOR_TYPES = {
    # Battery, as seen by the inverter
    "battery_soc": _soc("P071", multiplier=100.0),  # device sends 0.24 for 24 %
    "battery_power": _power("P069"),  # positive = discharging
    "battery_voltage": _voltage("P067"),
    "battery_current": _current("P068"),  # positive = discharging
    "battery_temperature": _temperature("P070"),
    "battery_charge_today": _energy("P075"),
    "battery_discharge_today": _energy("P076"),
    "battery_capacity": SensorSpec("P088", "Ah"),
    "battery_state": SensorSpec("P064"),  # 0 disconnected, 1 normal, 2 charging, 3 discharging
    # Grid
    "grid_power": _power("P644"),  # positive = import
    "grid_voltage_l1": _voltage("P044"),
    "grid_current_l1": _current("P045"),
    "grid_frequency": _frequency("P053"),
    "inverter_active_power": _power("P055"),
    "inverter_reactive_power": _reactive_power("P056"),
    "power_factor": SensorSpec("P057", None, DEVICE_CLASS_POWER_FACTOR, STATE_CLASS_MEASUREMENT, 2),
    "grid_import_total": _energy("P640"),
    "grid_export_total": _energy("P641"),
    # The meter only has lifetime counters, so "today" is the total minus its value at midnight
    "grid_import_midnight": _energy_snapshot("P642"),
    "grid_export_midnight": _energy_snapshot("P643"),
    # PV
    "pv_power": _power("P060"),  # DC coupled
    "pv_ac_power": _power("P237"),  # sign differs between firmware versions
    "pv_ac_power_l1": _power("P242"),
    "pv_ac_power_l2": _power("P243"),
    "pv_ac_power_l3": _power("P244"),
    "pv_energy_today": _energy("P061"),
    "pv_energy_total": _energy("P062"),
    "pv1_voltage": _voltage("P024"),
    "pv1_current": _current("P025", 2),
    "pv2_voltage": _voltage("P026"),
    "pv2_current": _current("P027", 2),
    "pv3_voltage": _voltage("P028"),
    "pv3_current": _current("P029", 2),
    # Backup output
    "eps_voltage": _voltage("P079"),
    "eps_current": _current("P080"),
    "eps_frequency": _frequency("P081"),
    "eps_power": _power("P082"),
    "eps_reactive_power": _reactive_power("P083"),
    "eps_energy_today": _energy("P084"),
    "eps_energy_total": _energy("P085"),
    # Settings (read only)
    "work_mode": SensorSpec("P651"),  # 1 self-consumption, 2 backup, 3 user-defined, 4 off-grid
    "charge_power_limit": _limit("L017"),
    "discharge_power_limit": _limit("L018"),
    "max_charge_soc": _percent("P647"),
    "min_soc_on_grid": _percent("P648"),
    "max_grid_charge_soc": _percent("L074"),
    # Device
    "rated_power": _limit("P005"),
    "phase_mode": SensorSpec("P000"),  # 0 single phase, 3 three phase
    "meter_type": SensorSpec("L034"),
    # Clock
    "utc_offset": SensorSpec("L096", UNIT_MINUTE),
}

# Named text sensors: name -> key
TEXT_SENSOR_TYPES = {
    "serial_number": "P002",
    "model": "P003",
    "brand": "P008",
    "firmware_main": "P006",
    "firmware_safety": "P007",
    "firmware_arm": "P139",
    "logger_firmware": "L023",
    "clock": "L094",
    "timezone": "L020",
}


def validate_key(value):
    value = cv.string_strict(value).upper()
    if not re.fullmatch(r"[A-Z][0-9]{2,4}", value):
        raise cv.Invalid("Hanchu keys look like P071 or L094")
    return value


def group_for(key, config):
    """Group name and whether it's fixed (explicit groups are never split)."""
    if CONF_GROUP in config:
        return f"user_{config[CONF_GROUP]}", True
    return KEY_TO_GROUP.get(key, f"other_{key[0]}"), False


def interval_ms(config, default_once):
    """Milliseconds, 0 for every poll, or None for once after boot."""
    if CONF_UPDATE_INTERVAL in config:
        return int(config[CONF_UPDATE_INTERVAL].total_milliseconds)
    return None if default_once else 0


def interval_expression(config, default_once):
    value = interval_ms(config, default_once)
    return ONCE_AFTER_BOOT if value is None else value


def platform_entries(config, types):
    """(key, entity config) for every named and custom entity in one platform block."""
    for name, spec in types.items():
        if name in config:
            yield (spec.key if isinstance(spec, SensorSpec) else spec), config[name]
    for custom in config.get(CONF_CUSTOM, []):
        yield custom[CONF_KEY], custom


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HanchuBle),
            cv.Optional(CONF_MAX_KEYS_PER_REQUEST, default=12): cv.int_range(min=1, max=64),
            # Replies take well under a second; stay below the loggers' ~10 s idle disconnect
            cv.Optional(CONF_REQUEST_TIMEOUT, default="5s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_POLL): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(HanchuBlePollTrigger),
                }
            ),
        }
    )
    .extend(cv.polling_component_schema("5s"))
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)


def _final_validate(config):
    full_config = fv.full_config.get()
    max_keys = config[CONF_MAX_KEYS_PER_REQUEST]
    groups = {}
    for domain, types, default_once in (
        ("sensor", SENSOR_TYPES, False),
        ("text_sensor", TEXT_SENSOR_TYPES, True),
    ):
        for platform_config in full_config.get(domain, []):
            if platform_config.get(CONF_PLATFORM) != "hanchu_ble":
                continue
            if platform_config[CONF_HANCHU_BLE_ID].id != config[CONF_ID].id:
                continue
            for key, entity in platform_entries(platform_config, types):
                if CONF_GROUP not in entity:
                    continue
                entry = groups.setdefault(entity[CONF_GROUP], {"keys": set(), "intervals": set()})
                entry["keys"].add(key)
                entry["intervals"].add(interval_ms(entity, default_once))

    for name, entry in groups.items():
        if len(entry["intervals"]) > 1:
            raise cv.Invalid(
                f"All keys in group '{name}' must use the same update_interval "
                "(text sensors default to once after boot)"
            )
        if len(entry["keys"]) > max_keys:
            raise cv.Invalid(
                f"Group '{name}' has {len(entry['keys'])} keys, "
                f"more than {CONF_MAX_KEYS_PER_REQUEST} ({max_keys})"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_max_keys_per_request(config[CONF_MAX_KEYS_PER_REQUEST]))
    cg.add(var.set_request_timeout(config[CONF_REQUEST_TIMEOUT]))
    for conf in config.get(CONF_ON_POLL, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
