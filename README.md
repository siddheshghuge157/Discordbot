# Discord LCD Monitor

An ESP32 desk monitor you can control from Discord. It reads temperature, humidity, light, a pot and an ultrasonic distance sensor, shows things on a 16x2 LCD, logs everything to ThingSpeak every 30 seconds, and takes chat commands through a Discord bot.

Full write-up: [docs/Report1_Discord_LCD_Monitor_1.docx](docs/Report1_Discord_LCD_Monitor_1.docx)

## Hardware

| Part | Pin |
|---|---|
| ESP32 dev board | - |
| 16x2 LCD via shift register | 17 (data), 16 (clock), 21 (enable) |
| DHT11 | 19 |
| HC-SR04 | trig 25, echo 26 |
| Potentiometer | 34 |
| LDR | 35 |
| LED | 13 |
| Buzzer | 15 |
| BOOT button (stopwatch) | 0 |

## Setup

1. Install these libraries from the Arduino Library Manager: ThingSpeak, DHT sensor library (plus Adafruit Unified Sensor), ArduinoJson, LiquidCrystal_SR.
2. Copy `secrets.example.h` to `secrets.h` and fill in your Wi-Fi, ThingSpeak channel/keys, Discord bot token and channel ID. `secrets.h` is git-ignored.
3. Pick the ESP32 board in the Arduino IDE and upload `discord_lcd_monitor.ino`.

## Discord commands

| Command | What it does |
|---|---|
| `!status` | live readings, thresholds, LED/buzzer state |
| `!temp <value>` / `!dist <value>` | set the alert thresholds |
| `!ledon` `!ledoff` `!buzzon` `!buzzoff` | manual toggles |
| `!history` | last 5 ThingSpeak temperature readings |
| `!read <field> <count>` | history for any ThingSpeak field (1-8) |
| `!lcd time / sensors / flash / stopwatch / timer` | change the LCD mode |
| `!sw start / stop / reset` | stopwatch (stopwatch mode only) |
| `!timer <seconds>` | countdown, up to 86400 s |
| `!timeroff` | silence the timer alarm |

The BOOT button also controls the stopwatch: short press starts/stops, long press resets.

## Good to know

- The Discord bot polls the channel every 3 seconds, so commands can take a moment.
- Alerts only fire once per event and re-arm when the reading goes back to normal.
- TLS certificate checking is turned off for Discord. Fine on a bench, not great long term.
- If a token ever ends up in a public repo, regenerate it in the Discord Developer Portal.
