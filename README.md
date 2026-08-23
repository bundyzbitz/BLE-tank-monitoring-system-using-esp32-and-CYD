# BLE-tank-monitoring-system-using-esp32-and-CYD
 
a tank monitor system designed for boats and RVs etc.

FEATURES

**Wireless Sensor Network**
 * Up to 9 independent tank sensors per system, each uniquely identified — no manual addressing or DIP switches
 * Battery-powered sensors with months-long runtime: deep sleep between readings, waking briefly on a configurable interval (default 60s, user-adjustable) to report and check for settings
 * No WiFi on the sensors — BLE-only for lower power draw and simpler, more robust wireless range
 * Self-calibrating — automatically learns each tank's empty/full range over time, with manual override and live raw sensor readout for hand-tuning
 * Multi-installation support via group labels — several independent TankMesh systems (e.g. neighboring boats at a marina) can coexist without ever showing each other's tanks
 * 
**Displays**
 * Two display formats: a 3.5" mains-style display and a 2.2" portable handheld, both running the same underlying system
 * Modern dark-themed UI with per-tank custom color coding, consistent across every screen and the mobile-style web portal
  *At-a-glance multi-tank overview — color-coded level bars for every tank on one screen
 * Detailed per-tank view — large graphical tank-fill indicator plus live percentage, battery status, and packet count
 * User-configurable viewing order — arrange tanks in whatever sequence makes sense for how each display is used
 * Zero-App Setup & Configuration
 * Entirely browser-based configuration — no app to install; connect to the display's own WiFi and configure from any phone or laptop
 * Remote sensor configuration over Bluetooth — rename, recolor, recalibrate, and adjust every sensor's settings without physically accessing it
 * Automatic sensor discovery — new sensors appear in the portal ready to be named and assigned

**Power Management**
 * Adjustable battery-saving screen timeout on the portable display — dims automatically when idle, wakes instantly on touch
 * Sensor battery monitoring with on-screen percentage
 * Fully configurable via the web portal — no code changes needed for day-to-day tuning
 * Engineered for Reliability
 * Dual-core architecture keeps the display responsive regardless of background wireless activity
 * Resilient to radio interference between WiFi and Bluetooth running simultaneously
  
sensors use a esp32S but you could use something like a esp32c3 supermini etc. only tested on the esp32S for now

All units use the Huge APP partition scheme, others could be used if the App partition is 2M or larger.

this is an early development project that ill be working on hopefully each week. I am using it atm to monitor tanks on my live aboard sail boat so im motivated to get it to 99%. (this is my first time opening a project up on something like github fyi)

I'm open to ideas / criticisms / branching the project or adding to my code, whatever, any input really

