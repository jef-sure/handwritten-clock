A weather clock in a handwritten font
=====================================

![A weather clock in a handwritten font](./clock.jpg "A weather clock in a handwritten font")

[Русская версия](./README-rus.md)

A clock on an ESP32 with a 320x240 screen. The time, the temperature and the
name of the city are written in a handwritten font made of Bézier curves: the
digits of the time flow one into another, the temperature and the city are
written stroke by stroke. The weather symbol is made of dots and flows smoothly
from one into another.

[![Video: the clock starts](./video-start.jpg)](https://youtu.be/5kWO57Guf_s)

This is the second version of a clock from 2021. The first one is described in
an [article on Habr](https://habr.com/ru/articles/818873/) (in Russian) and
stays under the tag `2021`. The idea and the layout of the screen come from it;
the code is written anew.

# What is on the screen

The time is at the top, the weather under it: a symbol and the temperature, and
the name of the city at the bottom.

  1. The digits of the time do not switch, they flow one into another;
  2. The temperature and the name of the city are written by a pen, letter by
     letter, joined up. The font has the Latin alphabet, the German "ä", "ö",
     "ü", "ß" and the Russian alphabet;
  3. The weather symbol is made of dots. When the weather changes, the dots of
     one symbol flow into the other;
  4. The time is silver by day and copper by night. The change starts half an
     hour before sunrise or sunset and ends half an hour after it;
  5. The color of the temperature and of the weather symbol depends on the
     temperature: 23 degrees Celsius are taken as the "comfortable" level,
     below it the color goes to blue, above it to red;
  6. After the start three signs in the top line show what the clock is still
     waiting for: the WiFi sign for the connection, "Z" for the time from the
     network, "W" for the weather. Once it has got what it waited for, a sign
     leaves.

The weather symbols flowing one into another, close up:

[![Video: weather symbols made of dots flow one into another](./video-dots.jpg)](https://youtu.be/bh8ax5zkCpk)

# Hardware

The board is a CYD (ESP32-2432S028), also known as the Cheap Yellow Display:
an ESP32 and a 2.8" 320x240 screen on the ILI9341 controller. Everything is
connected on it already. For another board the pins are set in
`main/display.c`:

| Display | Pin |
|---------|-----|
| MOSI    | 13  |
| MISO    | 12  |
| CLK     | 14  |
| CS      | 15  |
| DC      | 2   |
| BLK     | 21  |

The button is GPIO 0, the BOOT button of a CYD.

# Build

ESP-IDF 5.2 or newer is needed. I built and tested under 5.5.4 and 6.0.1, on
one board, and did not test under versions 5.2 to 5.4. The components,
[dgx](https://github.com/jef-sure/esp32-component-dgx) 0.5 or newer included, are
downloaded by the component manager.

    idf.py set-target esp32
    idf.py build
    idf.py -p /dev/ttyUSB0 flash monitor

The list of time zones is made during the build from the tz database of the
machine that builds: the script `misc/generate_tzones.py` takes it from
`/usr/share/zoneinfo` or, if there is no such directory, from the Python
package `tzdata`.

# Setup

The firmware holds neither the network nor the keys of the services: the clock
keeps them in NVS, and the settings survive reflashing. No computer is needed
for the setup.

## Network

A clock that knows no network shows a QR code. It is meant for the ESP BLE
Provisioning app by Espressif, available on Google Play and in the App Store:
the app reads the code from the screen and sends the name of the WiFi network
and the password to the clock over BLE. Having got the network, the clock
restarts and works without BLE from then on.

## Settings page

The BOOT button restarts the clock into the settings mode. How long it is
pressed does not matter. The clock connects to the network and shows the
address of the page as text and as a QR code:

![The settings screen: the address of the page as a QR code and as text](./settings-screen.jpg "The settings screen: the address of the page as a QR code and as text")

The page waits for 5 minutes, then the clock goes back to work. Pressing the
button again closes it too. Entering the settings on video:

[![Video: entering the settings](./video-settings.jpg)](https://youtu.be/48G7s-aJNIw)

The page itself:

<img src="./settings-page.png" alt="The settings page" width="320">

| Field | Meaning |
|-------|---------|
| Openweathermap.org API key | The key for the weather. It is free and is given out at [openweathermap.org](https://openweathermap.org/). Without it the clock shows the time only |
| Guess location from IP Info | Find the place by the IP address through [ipinfo.io](https://ipinfo.io). Off until it is turned on: an error of tens of kilometers is usual for this way |
| IP Info API key | The key of ipinfo.io, needed for this way only |
| Manual location | The place: coordinates ("52.52,13.40"), a city and a country ("Berlin,DE") or a postal code and a country ("10115,DE") |
| Name on the screen | A name of your own for the place on the screen. Empty: the name from the service |
| Manual timezone | The time zone. If none is chosen, the clock finds it by the place |
| Host name | The name in the network; under it the clock also answers as `<name>.local`. Before the setup it is `weather-clock-` and the last three bytes of the MAC address |
| Clear WiFi data | Forget the WiFi network: the clock shows the QR code for the setup from a phone again |
| Clear all settings | Forget everything: the network, the keys of the services, the place and its name. For giving the clock to somebody else |

Saved settings take effect with a restart.

The time comes from pool.ntp.org, the weather from OpenWeatherMap every 5
minutes.

# How it is made

Everything noticeable on the screen is done by
[dgx](https://github.com/jef-sure/esp32-component-dgx), my graphics library for
displays on microcontrollers: the handwritten font, writing at the pace of a
pen, morphing of the digits and of the dot symbols, 8-bit virtual screens with
a table of colors. Its repository has
[examples](https://github.com/jef-sure/esp32-component-dgx/tree/main/examples),
almost all of them built for the same CYD board.

The font is drawn in the [hw-fonts](https://github.com/jef-sure/hw-fonts)
editor: it keeps a letter as the path of a pen, not as an outline.

The settings page is an ordinary HTML file, `misc/index.html`. During the build
the script `misc/split_to_template.py` cuts it into pieces at places like
`%HOST_NAME%`, and the clock sends the page piece by piece straight from flash.

# Trade-offs

  1. There is no forecast, only the weather now;
  2. The time zone by the nearest place is sometimes wrong where a zone without
     summer time is near: El Paso gets the zone of Phoenix. Then the zone must
     be chosen on the page;
  3. Letters of other languages with diacritics are written without them in
     the name of the city: "Kraków" as "Krakow". The font has no CJK characters
     and no Arabic script: such a name is written in part or not written at
     all;
  4. The settings page is styled with Bootstrap, and the browser loads its CSS
     file from the Internet, from a CDN: the clock does not hold it. Without
     the Internet on the phone the page works but looks like bare HTML;
  5. The page has no password. It exists only after the button is pressed and
     only in the local network.

# Third-party resources

  1. The weather symbols are the [Weather Icons](https://github.com/erikflowers/weather-icons)
     font by Erik Flowers, licensed under SIL OFL 1.1. The font itself is in
     `misc`; `main/fonts` has a part of it turned into code by the `font2c`
     utility of dgx;
  2. JSON is parsed by [mjson](https://github.com/cesanta/mjson), Cesanta, MIT,
     as the component [jef-sure/mjson](https://components.espressif.com/components/jef-sure/mjson/).

# License

MIT, see [LICENSE](./LICENSE).
