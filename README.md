# BLE-tank-monitoring-system-using-esp32-and-CYD
 
a tank monitor system designed for boats and RVs etc.

features

* up to 9 tanks (could expand but needed a limit somewhere)
* sensors use analog input (rheostat/pressure/ultrasonic etc, as long as its an analog output)
* sensors use BLE and deep sleep so can be run off a battery like a 18650 for descent time frames
* choice of 1 or 2 screens or both.
* supports 2.2" and 3.5" CYD displays (2.2 for portable and 3.5 for a fixed display)
* android app will be coming but will be late in the development as I've not coded apps before
* all settings will be done from either the app or through either screen via a http AP interface 

sensors use a esp32S but you could use something like a esp32c3 supermini etc. only tested on the esp32S for now

this is an early development project that ill be working on hopefully each week. I am using it atm to monitor tanks on my live aboard sail boat so im motivated to get it to 99%. (this is my first time opening a project up on something like github fyi)

I'm open to ideas / criticisms / branching the project or adding to my code, whatever, any input really

