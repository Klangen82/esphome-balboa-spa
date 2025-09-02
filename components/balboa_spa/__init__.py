import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID
from esphome import pins  # <-- NYTT

DEPENDENCIES = ["uart"]

CONF_SPA_ID = "balboa_spa_id"
CONF_SPA_TEMP_SCALE = "spa_temp_scale"
CONF_ESPHOME_TEMP_SCALE = "esphome_temp_scale"
CONF_DIRECTION_PIN = "direction_pin"  # <-- NYTT

balboa_spa_ns = cg.esphome_ns.namespace("balboa_spa")
BalboaSpa = balboa_spa_ns.class_("BalboaSpa", cg.Component, uart.UARTDevice)

TEMP_SCALE = balboa_spa_ns.enum("TEMP_SCALE")
TEMP_SCALES = {
    254: TEMP_SCALE.UNDEFINED,
    "F": TEMP_SCALE.F,
    "C": TEMP_SCALE.C,
}

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BalboaSpa),
            cv.Optional(CONF_SPA_TEMP_SCALE, default=254): cv.enum(TEMP_SCALES, upper=True),
            cv.Optional(CONF_ESPHOME_TEMP_SCALE, default="C"): cv.enum(TEMP_SCALES, upper=True),
            cv.Optional(CONF_DIRECTION_PIN): pins.gpio_output_pin_schema,  # <-- NYTT
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)

    if spa_temp_scale_conf := config.get(CONF_SPA_TEMP_SCALE):
        cg.add(var.set_spa_temp_scale(spa_temp_scale_conf))

    if esphome_temp_scale_conf := config.get(CONF_ESPHOME_TEMP_SCALE):
        cg.add(var.set_esphome_temp_scale(esphome_temp_scale_conf))

    # <-- NYTT: skicka in pinnen till C++-klassen om den finns
    if CONF_DIRECTION_PIN in config:
        pin = yield cg.gpio_pin_expression(config[CONF_DIRECTION_PIN])
        cg.add(var.set_direction_pin(pin))

    yield uart.register_uart_device(var, config)
