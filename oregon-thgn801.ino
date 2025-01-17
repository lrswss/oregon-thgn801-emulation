/*
 * 
 * THGN801 emulation for Arduino Uno with FS1000A 433 RF-transmitter
 * 
 * (c) 2021 Lars Wessels <software@bytebox.org>
 *
 * Sketch to replace a (broken) THGN801 433MHz sensor sending temperature
 * and humidity readings using the Oregon V3 protocol. Tested with WMR200 
 * base station and rtl_433.
 * 
 * References:
 * http://wmrx00.sourceforge.net/Arduino/OregonScientific-RF-Protocols.pdf
 * https://github.com/merbanan/rtl_433/blob/master/src/devices/oregon_scientific.c
 *
 * Published under MIT license.
 * 
 */

#define RF433_TX_PIN 6  // output pin for 433MHz transmitter (e.g. FS1000A)
#define V3_CHANNEL 1  // 1..10 for WMR200
#define V3_PAYLOAD_NIBBLES 15 // excluding preamble, sync and trailing checksums
#define V3_TX_INTERVAL_SECS 53
#define V3_TX_BYTES 13

#define V3_PULSE_LENGHT_US 488  // data rate 1024Hz
#define V3_PULSE_SHORTEN_US 134 // 138us not working reliably
#define V3_PULSE_TUNING 1.1  // increase up to 1.4 if base station doesn't pick up messages

#define DEBUG_PAYLOAD
#define DEBUG_LASTTX
#define DEBUG_INC

// set THGN801's values for temperature (degrees Celcius) and humidity 
// which are  increased after each transmission if DEBUG_INC is set
static float t_temp = -1.5;
static uint8_t t_hum = 20;

// rolling code changes on every sensor reset
static uint8_t rcode;


// calculate a bitwise crc8 ccitt checksum
uint8_t crc8_checksum_v3(uint8_t *payload, uint8_t payloadNibbles) {
    uint8_t crc = 0x00;  // crc8 init value

    // iterate over nibbles with sensor payload including a trailing iteration
    for (uint8_t i = 7; i <= (payloadNibbles + 7); i++) {
        if (i < (payloadNibbles + 7)) {
          if ((i % 2) == 0)
              crc ^= payload[i/2] >> 4;
          else
              crc ^= (payload[i/2] & 0x0F);
        }
        for (uint8_t j = 0; j < 4; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else 
                crc <<= 1;
        }
    }
    
    // nibbles need to be swapped (LSD)
    return ((crc & 0x0F) << 4) | ((crc & 0xF0) >> 4);
}


// calculate the Oregon V3 checksum
// based on https://github.com/merbanan/rtl_433/blob/master/src/devices/oregon_scientific.c
uint8_t oregon_checksum_v3(uint8_t *payload, int payloadNibbles) {
    uint8_t checksum;
  
    // remove sync byte '0xA' from first sensor data nibble
    checksum = (payload[3] & 0xF);

    // nibbles used for simple checksum (sum of nibbles)
    // 4 (sensor id) + 1 (V3_CHANNEL) + 2 (rolling code) + 1 (flag) + payloadNibbles
    for (uint8_t i = 4; i < ((28 + payloadNibbles*4) / 8); i++)
        checksum += (payload[i] >> 4) + (payload[i] & 0xF);

    if ((payloadNibbles % 2) == 0)
        checksum += payload[((28 + payloadNibbles*4)/8)] >> 4;

    return ((checksum & 0x0F) << 4) | ((checksum & 0xF0) >> 4);
}


// send a single byte as series of manchester encoded RF pulses
// if base station won't receive packets timing is probably off
// adjusting V3_PULSE_TUNING from 1.1 up to 1.4 should help
void manchester_encode_v3(uint8_t txByte) {
    static uint32_t txMicros = micros();
    uint8_t bitMask = 0; 

    // send 8 bits of data 
    for (uint8_t i = 0; i < 8; i++) {  

        // ensure equal distant bit pulses
        txMicros += (V3_PULSE_LENGHT_US * 2);
        if (txMicros - micros() < 0)
            delayMicroseconds((txMicros - micros()) * -1);
        else
            delayMicroseconds(txMicros - micros());

        // send bit 4..7 first followed by bit 0..3
        if (!bitMask) 
            bitMask = 0x10; // start with bit 4
        else if (bitMask == 0x80) 
            bitMask = 0x01; // jump from bit 7 to bit 0
        else
            bitMask <<= 1; // next bit
        
        //  high bit is encoded as as on-to-off and low bit as off-to-on transition
        digitalWrite(RF433_TX_PIN, ((txByte & bitMask) >= 1) ? HIGH : LOW);
        delayMicroseconds((V3_PULSE_LENGHT_US * V3_PULSE_TUNING) - V3_PULSE_SHORTEN_US);
        digitalWrite(RF433_TX_PIN, ((txByte & bitMask) >= 1) ? LOW : HIGH);
    }
}  


// returns payload for THGN801 outdoor sensor (13 bytes)
uint8_t *payload_thgn801(uint8_t rollingCode, float tempC, uint8_t hum) {
    static uint8_t payload[V3_TX_BYTES];
    uint16_t t10;

    // 6 nibbles preamble
    // 1 nibble sync
    // 4 nibbles sensor id
    // 1 nibble channel
    // 2 nibbles rolling code
    // 1 nibble flag (battery)
    // n nibbles sensor specific payload (THGN801 n=7, 28 bits)
    // 2 nibbles oregon checksum
    // 2 nibbles crc8 checksum
    memset(payload, 0, V3_TX_BYTES);
  
    // preamble with 1-bit pulses (6 nibbles) and 1 sync nibble '0101' (28 bits)
    memset(payload, 0xFF, 3);  
    payload[3] = 0xA0;
  
    // nibbles 8..11: oregon sensor id (16 bits)
    payload[3] |= 0x0F; // THGN801 is 0xF824
    payload[4] = 0x82;
    payload[5] = 0x40;
  
    // nibble 12: channel 1..10 for THGN801 using a WMR200 base station (4 bits)
    payload[5] |= V3_CHANNEL > 15 ? 15 : V3_CHANNEL;
  
    // nibble 13..14 is rolling code, changes on sensor reset (16 bits LSD) 
    payload[6] = rollingCode;
  
    // nibble 15 is flag for battery status (4 bits)
    payload[7] = 0x00; // set to 0x40 for low battery

    // nibbles 16..22 encode THGN801 specific data (24 bits)
    // 16..19 temperature in degC as LSD with 0.1 precision
    t10 = abs(tempC * 10.001);  // .001 required to fix rounding issues
    payload[7] |= ((t10 % 10) & 0x0F);
    payload[8] = ((t10 / 10) % 10) << 4 | ((t10 / 100) & 0x0F);
    payload[9] = (tempC < 0.0) ? 0x80 : 0; // nibble 19 encodes temperature sign (0x80 => neg.)    

    // 20..21 encodes relative humidity
    payload[9] |= (hum % 10) & 0x0F; // 0-100%
    payload[10] = ((hum / 10) % 10) << 4;  // use of nibble 22 unknown

    // checksum (8 bits) for nibbles 16..21 with sensor specific data (sum of nibbles)
    payload[11] = oregon_checksum_v3(payload, V3_PAYLOAD_NIBBLES);

    // crc8 checksum for V3 protocol (8 bits)
    payload[12] = crc8_checksum_v3(payload, V3_PAYLOAD_NIBBLES);

    return payload;  
}


// send manchester encoded payload for a Oregon V3 sensor
void send_data_v3(uint8_t *payload, uint8_t len) {
    uint8_t i;
#ifdef DEBUG_PAYLOAD
    Serial.print(millis());
    Serial.print(": V3 Payload [ ");
    for (i = 0; i < len; i++) {
        if (payload[i] < 16)
            Serial.print("0");
        Serial.print(payload[i], HEX);
        Serial.print(" ");
    }
    Serial.println("]");
#endif
    digitalWrite(RF433_TX_PIN, LOW);
    for (i = 0; i < len; i++)
        manchester_encode_v3(payload[i]);

    // need to add extra delay after last low to high 
    // pulse since there is no more data to come...
    if ((payload[i] & 0x08) == 0)
        delayMicroseconds(V3_PULSE_LENGHT_US);

    digitalWrite(RF433_TX_PIN, LOW);
}


void setup() {  
    pinMode(RF433_TX_PIN, OUTPUT);
    pinMode(LED_BUILTIN, OUTPUT);

    Serial.begin(115200);
    delay(250);   
    Serial.print(F("\n\nStarting Oregon V3 THGN801 emulator...\n\n"));
    
    randomSeed(analogRead(1)); // random seed for rolling code
    rcode = random(0x01, 0xFE);
    Serial.print(millis());
    Serial.print(": Rolling code: ");
    Serial.print(((rcode & 0x0F) << 4) | ((rcode & 0xF0) >> 4));
    Serial.print(", Channel: ");
    Serial.print(V3_CHANNEL);
    Serial.print(", RF433 packets: ");
    Serial.println(V3_TX_BYTES);
}


void loop() {
    static uint32_t seconds = 0;
    static uint32_t lastMillis = 0, lastTX = 0;
    static bool tx_pending = true;
    
    if (millis() - lastMillis > 1000) {
        digitalWrite(LED_BUILTIN, LOW);
        lastMillis = millis();
        seconds++;
        Serial.print(millis());
        Serial.println(": Waiting for next TX...");
        tx_pending = true;
    }

    // Oregon V3 base station is expecting to 
    // receive readings at fixed intervals
    if (seconds % V3_TX_INTERVAL_SECS == 0 && tx_pending) {
        tx_pending = false;
        digitalWrite(LED_BUILTIN, HIGH);

        Serial.print(millis());
        Serial.print(": THGN801 (Temperature: ");
        Serial.print(t_temp);
        Serial.print(" °C, Humidity: ");
        Serial.print(t_hum);
        Serial.print(" %");
#ifdef DEBUG_LASTTX
        if (lastTX > 0) {
            Serial.print(", Last TX ");
            Serial.print(millis() - lastTX);
            Serial.print(" ms ago");
            lastTX = millis();
        }
#endif
        Serial.println(")");

        // create and send off sensor payload
        send_data_v3(payload_thgn801(rcode, t_temp, t_hum), V3_TX_BYTES);

#ifdef DEBUG_INC
        // increase readings for testing
        t_temp += 0.1;
        if (t_temp  > 45)
            t_temp = 19.0;
        t_hum += 1;
        if (t_hum > 95)
            t_hum = 30;
#endif
    }
}
