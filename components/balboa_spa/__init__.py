esphome:
  name: balboa-spa
  friendly_name: Balboa Spa

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

logger:
  baud_rate: 0  # inga logs på HW-UART

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "Balboa-Spa Fallback"
    password: "QmVJAdTQzZvp"

api:
  encryption:
    key: "kH1yCrws6jbWSffQPqVN6BN4fUAHREWDZoJL9wPAOd0="
ota:
  - platform: esphome
    password: "a0bdec74b585e596bea1ca1ddb3fa12b"

# Använder din lokala custom_component (ingen external_components behövs!)

uart:
  id: spa_uart_bus
  tx_pin: GPIO17
  rx_pin: GPIO16
  baud_rate: 115200
  parity: NONE
  stop_bits: 1
  rx_buffer_size: 128

balboa_spa:
  id: spa
  spa_temp_scale: C
  direction_pin: GPIO14   # DE + /RE ihop till denna pin

switch:
  - platform: balboa_spa
    balboa_spa_id: spa
    jet1:
      name: "Jet 1"
    jet2:
      name: "Jet 2"
    jet3:
      name: "Jet 3"
    jet4:
      name: "Jet 4"
    light:
      name: "Spa Light"
    blower:
      name: "Blower"

climate:
  - platform: balboa_spa
    balboa_spa_id: spa
    name: "Spa Thermostat"
    visual:
      min_temperature: 7 °C
      max_temperature: 40 °C
      temperature_step: 0.5 °C

binary_sensor:
  - platform: balboa_spa
    balboa_spa_id: spa
    blower:
      name: "Blower"
    highrange:
      name: "High Range"
    circulation:
      name: "Circulation Pump"
    restmode:
      name: "Rest Mode"
    heatstate:
      name: "Heat State"
    connected:
      name: "Balboa Connected"
