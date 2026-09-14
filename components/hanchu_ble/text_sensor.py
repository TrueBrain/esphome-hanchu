import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_GROUP, CONF_KEY, CONF_UPDATE_INTERVAL

from . import (
    CONF_CUSTOM,
    CONF_HANCHU_BLE_ID,
    TEXT_SENSOR_TYPES,
    HanchuBle,
    group_for,
    interval_expression,
    validate_key,
)

DEPENDENCIES = ["hanchu_ble"]

ENTITY_OPTIONS = {
    cv.Optional(CONF_GROUP): cv.string_strict,
    # Default: once after boot, retried every poll until the device answers
    cv.Optional(CONF_UPDATE_INTERVAL): cv.positive_time_period_milliseconds,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HANCHU_BLE_ID): cv.use_id(HanchuBle),
        **{
            cv.Optional(name): text_sensor.text_sensor_schema().extend(ENTITY_OPTIONS)
            for name in TEXT_SENSOR_TYPES
        },
        cv.Optional(CONF_CUSTOM): cv.ensure_list(
            text_sensor.text_sensor_schema().extend({cv.Required(CONF_KEY): validate_key, **ENTITY_OPTIONS})
        ),
    }
)


async def _register(parent, key, config):
    sens = await text_sensor.new_text_sensor(config)
    group, strict = group_for(key, config)
    interval = interval_expression(config, default_once=True)
    cg.add(parent.register_text_sensor(key, group, strict, interval, sens))


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HANCHU_BLE_ID])
    for name, key in TEXT_SENSOR_TYPES.items():
        if name in config:
            await _register(parent, key, config[name])
    for custom in config.get(CONF_CUSTOM, []):
        await _register(parent, custom[CONF_KEY], custom)
