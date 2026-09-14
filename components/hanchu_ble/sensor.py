import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import CONF_GROUP, CONF_KEY, CONF_UPDATE_INTERVAL

from . import (
    CONF_CUSTOM,
    CONF_HANCHU_BLE_ID,
    SENSOR_TYPES,
    HanchuBle,
    group_for,
    interval_expression,
    validate_key,
)

DEPENDENCIES = ["hanchu_ble"]

ENTITY_OPTIONS = {
    cv.Optional(CONF_GROUP): cv.string_strict,
    # Default: every poll of the parent hanchu_ble
    cv.Optional(CONF_UPDATE_INTERVAL): cv.positive_time_period_milliseconds,
}


def _named_schema(spec):
    defaults = {"accuracy_decimals": spec.accuracy_decimals}
    if spec.unit is not None:
        defaults["unit_of_measurement"] = spec.unit
    if spec.device_class is not None:
        defaults["device_class"] = spec.device_class
    if spec.state_class is not None:
        defaults["state_class"] = spec.state_class
    return sensor.sensor_schema(**defaults).extend(ENTITY_OPTIONS)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HANCHU_BLE_ID): cv.use_id(HanchuBle),
        **{cv.Optional(name): _named_schema(spec) for name, spec in SENSOR_TYPES.items()},
        cv.Optional(CONF_CUSTOM): cv.ensure_list(
            sensor.sensor_schema(accuracy_decimals=2).extend(
                {cv.Required(CONF_KEY): validate_key, **ENTITY_OPTIONS}
            )
        ),
    }
)


async def _register(parent, key, config, multiplier):
    sens = await sensor.new_sensor(config)
    group, strict = group_for(key, config)
    interval = interval_expression(config, default_once=False)
    cg.add(parent.register_sensor(key, group, strict, interval, multiplier, sens))


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HANCHU_BLE_ID])
    for name, spec in SENSOR_TYPES.items():
        if name in config:
            await _register(parent, spec.key, config[name], spec.multiplier)
    for custom in config.get(CONF_CUSTOM, []):
        await _register(parent, custom[CONF_KEY], custom, 1.0)
