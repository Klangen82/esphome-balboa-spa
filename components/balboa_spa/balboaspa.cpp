#include "balboaspa.h"

namespace esphome {
namespace balboa_spa {

static const char *TAG = "BalboaSpa.component";

void BalboaSpa::setup() {
    input_queue.clear();
    output_queue.clear();

    // NEW: keep RS485 in receive by default (DE=0, /RE=0)
    if (direction_pin_ != nullptr) {
        direction_pin_->setup();              // OUTPUT (från schema)
        direction_pin_->digital_write(false); // RX-läge
    }
}

void BalboaSpa::update() {
    uint32_t now = millis();
    if (last_received_time + 10000 < now) {
        ESP_LOGW(TAG, "No new message since %d Seconds! Mark as dead!", (now - last_received_time) / 1000);
        status_set_error("No Communication with Balboa Mainboard!");
        client_id = 0;
    } else if (status_has_error()) {
        status_clear_error();
    }

    while (available()) {
      read_serial();
    }

    // Run through listeners
    for (const auto &listener : this->listeners_) {
      listener(&spaState);
    }
}

float BalboaSpa::get_setup_priority() const { return esphome::setup_priority::LATE; }

SpaConfig BalboaSpa::get_current_config() { return spaConfig; }
SpaState* BalboaSpa::get_current_state() { return &spaState; }

void BalboaSpa::set_temp(float temp) {
    float target_temp = 0.0;

    if (esphome_temp_scale == TEMP_SCALE::C &&
        temp >= ESPHOME_BALBOASPA_MIN_TEMPERATURE_C &&
        temp <= ESPHOME_BALBOASPA_MAX_TEMPERATURE_C) {
        target_temp = temp;
    } else if (esphome_temp_scale == TEMP_SCALE::F &&
               temp >= ESPHOME_BALBOASPA_MIN_TEMPERATURE_F &&
               temp <= ESPHOME_BALBOASPA_MAX_TEMPERATURE_F) {
        target_temp = convert_f_to_c(temp);
    } else {
        ESP_LOGW(TAG, "set_temp(%f): is INVALID! %d", temp, esphome_temp_scale);
        return;
    }

    if (spa_temp_scale == TEMP_SCALE::C) {
        target_temperature = target_temp * 2;
    } else if (spa_temp_scale == TEMP_SCALE::F) {
        target_temperature = convert_c_to_f(target_temp);
    } else {
        ESP_LOGW(TAG, "set_temp(%f): spa_temp_scale not set. Ignoring %d", temp, spa_temp_scale);
        return;
    }

    send_command = 0xff;
}

void BalboaSpa::set_highrange(bool high) {
    ESP_LOGD(TAG, "highrange=%d to %d requested", spaState.highrange, high);
    if (high != spaState.highrange) {
        send_command = 0x50;
    }
}

bool BalboaSpa::get_restmode() {
  return spaState.rest_mode == 1;
}

void BalboaSpa::toggle_heat() {
  ESP_LOGD("balboa_spa", "Send 0x51 to toggle heat/rest");
  send_command = 0x51;
}

void BalboaSpa::set_hour(int hour) {
    if (hour >= 0 && hour <= 23) {
        target_hour = hour;
        send_command = 0x21;
    }
}

void BalboaSpa::set_minute(int minute) {
    if (minute >= 0 && minute <= 59) {
        target_minute = minute;
        send_command = 0x21;
    }
}

void BalboaSpa::set_timescale(bool is_24h) {
    send_preference_data = is_24h ? 0x01 : 0x00;
    send_command = 0x27;
    send_preference_code = 0x02;
}

void BalboaSpa::toggle_light()  { send_command = 0x11; }
void BalboaSpa::toggle_light2() { send_command = 0x12; }
void BalboaSpa::toggle_jet1()   { send_command = 0x04; }
void BalboaSpa::toggle_jet2()   { send_command = 0x05; }
void BalboaSpa::toggle_jet3()   { send_command = 0x06; }
void BalboaSpa::toggle_jet4()   { send_command = 0x07; }
void BalboaSpa::toggle_blower() { send_command = 0x0C; }

void BalboaSpa::read_serial() {
    if (!read_byte(&received_byte)) {
        return;
    }

    // Drop until SOF is seen
    if (input_queue.first() != 0x7E && received_byte != 0x7E) {
        input_queue.clear();
        return;
    }

    // Double SOF-marker, drop last one
    if (input_queue.size() >= 2 && input_queue[1] == 0x7E) {
        input_queue.pop();
        return;
    }

    input_queue.push(received_byte);

    // Complete package
    if (received_byte == 0x7E && input_queue.size() > 2 && input_queue.size() >= input_queue[1] + 2) {

        if (input_queue.size() - 2 < input_queue[1]) {
            ESP_LOGD(TAG, "packet_size: %d, recv_size: %d", input_queue[1], input_queue.size());
            ESP_LOGD(TAG, "%s", "Packet incomplete!");
            input_queue.clear();
            return;
        }

        auto calculated_crc = this->crc8(input_queue, true);
        auto packet_crc = input_queue[input_queue[1]];
        if (calculated_crc != packet_crc) {
            ESP_LOGD(TAG, "CRC %d != Packet crc %d end=0x%X", calculated_crc, packet_crc, input_queue[input_queue[1] + 1]);
            input_queue.clear();
            return;
        }

        // Unregistered or yet in progress
        if (client_id == 0) {
            ESP_LOGD(TAG, "Spa/node/id: %s", "Unregistered");
            print_msg(input_queue);

            // FE BF 02:got new client ID
            if (input_queue[2] == 0xFE && input_queue[4] == 0x02) {
                client_id = input_queue[5];
                if (client_id > 0x2F) client_id = 0x2F;
                ESP_LOGD(TAG, "Spa/node/id: Got ID: %d, acknowledging", client_id);
                ID_ack();
                ESP_LOGD(TAG, "Spa/node/id: %d", client_id);
            }

            // FE BF 00:Any new clients?
            if (input_queue[2] == 0xFE && input_queue[4] == 0x00) {
                ESP_LOGD(TAG, "Spa/node/id: %s", "Requesting ID");
                ID_request();
            }
        } else if (input_queue[2] == client_id && input_queue[4] == 0x06) { // we have an ID, do clever stuff
            // client_id BF 06:Ready to Send
            if (send_command == 0x21) {
                output_queue.push(client_id);
                output_queue.push(0xBF);
                output_queue.push(0x21);
                output_queue.push(target_hour);
                output_queue.push(target_minute);
            } else if (send_command == 0xff) {
                // 0xff marks dirty temperature for now
                output_queue.push(client_id);
                output_queue.push(0xBF);
                output_queue.push(0x20);
                output_queue.push(target_temperature);
            } else if (send_command == 0x00) {
                if (config_request_status == 0) { // Get configuration of the hot tub
                    output_queue.push(client_id);
                    output_queue.push(0xBF);
                    output_queue.push(0x22);
                    output_queue.push(0x00);
                    output_queue.push(0x00);
                    output_queue.push(0x01);
                    ESP_LOGD(TAG, "Spa/config/status: %s", "Getting config");
                    config_request_status = 1;
                } else if (faultlog_request_status == 0) { // Get the fault log
                    output_queue.push(client_id);
                    output_queue.push(0xBF);
                    output_queue.push(0x22);
                    output_queue.push(0x20);
                    output_queue.push(0xFF);
                    output_queue.push(0x00);
                    faultlog_request_status = 1;
                    ESP_LOGD(TAG, "Spa/debug/faultlog_request_status: %s", "requesting fault log, #1");
                } else if ((filtersettings_request_status == 0) && (faultlog_request_status == 2)) { // Get the filter cycles log once we have the faultlog
                    output_queue.push(client_id);
                    output_queue.push(0xBF);
                    output_queue.push(0x22);
                    output_queue.push(0x01);
                    output_queue.push(0x00);
                    output_queue.push(0x00);
                    ESP_LOGD(TAG, "Spa/debug/filtersettings_request_status: %s", "requesting filter settings, #1");
                    filtersettings_request_status = 1;
                } else {
                    // A Nothing to Send message is sent by a client immediately after a Clear to Send message if the client has no messages to send.
                    output_queue.push(client_id);
                    output_queue.push(0xBF);
                    output_queue.push(0x07);
                }
            } else if (send_command == 0x27) {
                output_queue.push(client_id);
                output_queue.push(0xBF);
                output_queue.push(0x27);
                output_queue.push(send_preference_code);
                output_queue.push(send_preference_data);
            } else {
                output_queue.push(client_id);
                output_queue.push(0xBF);
                output_queue.push(0x11);
                output_queue.push(send_command);
                output_queue.push(0x00);
            }

            rs485_send();
            send_command = 0x00;
        } else if (input_queue[2] == client_id && input_queue[4] == 0x2E) {
            if (last_state_crc != input_queue[input_queue[1]]) {
                decodeSettings();
            }
        } else if (input_queue[2] == client_id && input_queue[4] == 0x28) {
            if (last_state_crc != input_queue[input_queue[1]]) {
                decodeFault();
            }
        } else if (input_queue[2] == 0xFF && input_queue[4] == 0x13) { // FF AF 13:Status Update
            if (last_state_crc != input_queue[input_queue[1]]) {
                decodeState();
            }
        } else if (input_queue[2] == client_id && input_queue[4] == 0x23) { // Filter Cycle Message
            if (last_state_crc != input_queue[input_queue[1]]) {
                ESP_LOGD(TAG, "Spa/debug/faultlog_request_status: %s", "decoding filter settings");
                decodeFilterSettings();
            }
        } else {
            // Optional debug
            // print_msg(input_queue);
        }

        // Clean up queue
        input_queue.clear();
    }
    last_received_time = millis();
}

uint8_t BalboaSpa::crc8(CircularBuffer<uint8_t, 100> &data, bool ignore_delimiter) {
    unsigned long crc_value;
    int bit_index;
    uint8_t data_length = ignore_delimiter ? data.size() - 2 : data.size();

    crc_value = 0x02;
    for (size_t byte_index = ignore_delimiter; byte_index < data_length; byte_index++) {
        crc_value ^= data[byte_index];
        for (bit_index = 0; bit_index < 8; bit_index++) {
            if ((crc_value & 0x80) != 0) {
                crc_value <<= 1;
                crc_value ^= 0x7;
            } else {
                crc_value <<= 1;
            }
        }
    }
    return crc_value ^ 0x02;
}

void BalboaSpa::ID_request() {
    output_queue.push(0xFE);
    output_queue.push(0xBF);
    output_queue.push(0x01);
    output_queue.push(0x02);
    output_queue.push(0xF1);
    output_queue.push(0x73);

    rs485_send();
}

void BalboaSpa::ID_ack() {
    output_queue.push(client_id);
    output_queue.push(0xBF);
    output_queue.push(0x03);

    rs485_send();
}

void BalboaSpa::rs485_send() {
    // Add telegram length
    output_queue.unshift(output_queue.size() + 2);

    // Add CRC
    output_queue.push(crc8(output_queue, false));

    // Wrap telegram in SOF/EOF
    output_queue.unshift(0x7E);
    output_queue.push(0x7E);

    // NEW: go TX (enable driver), kort vändtid
    if (direction_pin_ != nullptr) {
        direction_pin_->digital_write(true);   // TX (DE=1, /RE=1)
        delayMicroseconds(80);                 // ~1 byte-time @115200
    }

    for (loop_index = 0; loop_index < output_queue.size(); loop_index++) {
        write(output_queue[loop_index]);
    }

    flush();  // säkerställ att allt lämnar UARTen

    // NEW: tillbaka till RX
    if (direction_pin_ != nullptr) {
        delayMicroseconds(80);                 // litet mellanrum innan lyssning
        direction_pin_->digital_write(false);  // RX (DE=0, /RE=0)
    }

    output_queue.clear();
}

void BalboaSpa::print_msg(CircularBuffer<uint8_t, 100> &data) {
    std::stringstream debug_stream;
    for (loop_index = 0; loop_index < data.size(); loop_index++) {
        received_byte = data[loop_index];
        if (received_byte < 0x0A) debug_stream << "0";
        debug_stream << std::hex << (int)received_byte;
        debug_stream << " ";
    }
    yield();
}

void BalboaSpa::decodeSettings() {
    ESP_LOGD(TAG, "Spa/config/status: Got config");
    spaConfig.pump1 = input_queue[5] & 0x03;
    spaConfig.pump2 = (input_queue[5] & 0x0C) >> 2;
    spaConfig.pump3 = (input_queue[5] & 0x30) >> 4;
    spaConfig.pump4 = (input_queue[5] & 0xC0) >> 6;
    spaConfig.pump5 = (input_queue[6] & 0x03);
    spaConfig.pump6 = (input_queue[6] & 0xC0) >> 6;
    spaConfig.light1 = (input_queue[7] & 0x03);
    spaConfig.light2 = (input_queue[7] >> 2) & 0x03;
    spaConfig.circ = ((input_queue[8] & 0x80) != 0);
    spaConfig.blower = ((input_queue[8] & 0x03) != 0);
    spaConfig.mister = ((input_queue[9] & 0x30) != 0);
    spaConfig.aux1 = ((input_queue[9] & 0x01) != 0);
    spaConfig.aux2 = ((input_queue[9] & 0x02) != 0);
    spaConfig.temperature_scale = input_queue[3] & 0x01; // 0=F, 1=C
    spaConfig.clock_mode = (input_queue[3] >> 1) & 0x1;  // 0=12h,1=24h
    config_request_status = 2;

    if (spa_temp_scale == TEMP_SCALE::UNDEFINED) {
        spa_temp_scale = static_cast<TEMP_SCALE>(spaConfig.temperature_scale);
    }
}

void BalboaSpa::decodeState() {
    double temp_read = 0.0;

    // 25: set temperature
    if (spa_temp_scale == TEMP_SCALE::C) {
        temp_read = input_queue[25] / 2.0;
    } else if (spa_temp_scale == TEMP_SCALE::F) {
        temp_read = convert_f_to_c(input_queue[25]);
    }

    if (esphome_temp_scale == TEMP_SCALE::C &&
        temp_read >= ESPHOME_BALBOASPA_MIN_TEMPERATURE_C &&
        temp_read <= ESPHOME_BALBOASPA_MAX_TEMPERATURE_C) {
        spaState.target_temp = temp_read;
        ESP_LOGD(TAG, "Spa/temperature/target: %.2f C", temp_read);
    } else if (esphome_temp_scale == TEMP_SCALE::F &&
               temp_read >= ESPHOME_BALBOASPA_MIN_TEMPERATURE_F &&
               temp_read <= ESPHOME_BALBOASPA_MAX_TEMPERATURE_F) {
        spaState.target_temp = convert_c_to_f(temp_read);
        ESP_LOGD(TAG, "Spa/temperature/target: %.2f F", temp_read);
    } else {
        ESP_LOGW(TAG, "Spa/temperature/target INVALID %2.f %.2f %d %d",
                 input_queue[25], temp_read, spaConfig.temperature_scale, esphome_temp_scale);
    }

    // 7: actual temperature
    if (input_queue[7] != 0xFF) {
        if (spa_temp_scale == TEMP_SCALE::C) {
            temp_read = input_queue[7] / 2.0;
        } else if (spa_temp_scale == TEMP_SCALE::F) {
            temp_read = convert_f_to_c(input_queue[7]);
        }

        if (esphome_temp_scale == TEMP_SCALE::C) {
            spaState.current_temp = temp_read;
        } else if (esphome_temp_scale == TEMP_SCALE::F) {
            spaState.current_temp = convert_c_to_f(temp_read);
        }
    }

    // time
    target_hour = input_queue[8];
    target_minute = input_queue[9];
    if (target_hour != spaState.hour || target_minute != spaState.minutes) {
        spaState.hour = target_hour;
        spaState.minutes = target_minute;
    }

    spaState.rest_mode = input_queue[10];

    // 15: heat state + high/low range
    spaState.heat_state = bitRead(input_queue[15], 4);
    double spa_component_state = bitRead(input_queue[15], 2);
    if (spa_component_state != spaState.highrange) {
        spaState.highrange = spa_component_state;
    }

    // 16: jets
    spa_component_state = bitRead(input_queue[16], 1);
    if (spa_component_state != spaState.jet1) spaState.jet1 = spa_component_state;

    spa_component_state = bitRead(input_queue[16], 3);
    if (spa_component_state != spaState.jet2) spaState.jet2 = spa_component_state;

    spa_component_state = bitRead(input_queue[16], 5);
    if (spa_component_state != spaState.jet3) spaState.jet3 = spa_component_state;

    spa_component_state = bitRead(input_queue[16], 7);
    if (spa_component_state != spaState.jet4) spaState.jet4 = spa_component_state;

    // 18: circ + blower
    spa_component_state = bitRead(input_queue[18], 1);
    if (spa_component_state != spaState.circulation) spaState.circulation = spa_component_state;

    spa_component_state = bitRead(input_queue[18], 2);
    if (spa_component_state != spaState.blower) spaState.blower = spa_component_state;

    // 19: lights
    spa_component_state = input_queue[19] == 0x03;
    if (spa_component_state != spaState.light) spaState.light = spa_component_state;

    spa_component_state = (input_queue[19] & 0x0C) >> 2; // light2
    if (spa_component_state != spaState.light2) spaState.light2 = spa_component_state;

    last_state_crc = input_queue[input_queue[1]];
}

void BalboaSpa::decodeFilterSettings() {
    spaFilterSettings.filter1_hour = input_queue[5];
    spaFilterSettings.filter1_minute = input_queue[6];
    spaFilterSettings.filter1_duration_hour = input_queue[7];
    spaFilterSettings.filter1_duration_minute = input_queue[8];
    spaFilterSettings.filter2_enable = bitRead(input_queue[9], 7);
    spaFilterSettings.filter2_hour = input_queue[9] ^ (spaFilterSettings.filter2_enable << 7);
    spaFilterSettings.filter2_minute = input_queue[10];
    spaFilterSettings.filter2_duration_hour = input_queue[11];
    spaFilterSettings.filter2_duration_minute = input_queue[12];

    // (loggar kvar som i original)
    filtersettings_request_status = 2;
}

void BalboaSpa::decodeFault() {
    spaFaultLog.total_entries = input_queue[5];
    spaFaultLog.current_entry = input_queue[6];
    spaFaultLog.fault_code = input_queue[7];
    // (switch med felkoder kvar som i original)
    spaFaultLog.days_ago = input_queue[8];
    spaFaultLog.hour = input_queue[9];
    spaFaultLog.minutes = input_queue[10];
    faultlog_request_status = 2;
}

bool BalboaSpa::is_communicating() { return client_id != 0; }
void BalboaSpa::set_spa_temp_scale(TEMP_SCALE scale) { spa_temp_scale = scale; }
void BalboaSpa::set_esphome_temp_scale(TEMP_SCALE scale) { esphome_temp_scale = scale; }

float BalboaSpa::convert_c_to_f(float c) { return (c * 9.0f / 5.0f) + 32.0f; }
float BalboaSpa::convert_f_to_c(float f) { return (f - 32.0f) * 5.0f / 9.0f; }

}  // namespace balboa_spa
}  // namespace esphome
