# HP Omen Keyboard backlight controller.
This kernel module exposes a multicolor led device for each of the 4 keyboard zones.
Note: some parts(wmi query function) are taken from hp-wmi driver.

## Usage
``` 
    echo "R G B" >> /sys/class/leds/keyboard\:rgb\:zone0/multi_intensity 
```
### Example 
Make all leds white.
```
    echo "255 255 255" >> /sys/class/leds/keyboard\:rgb\:zone0/multi_intensity 
    echo "255 255 255" >> /sys/class/leds/keyboard\:rgb\:zone1/multi_intensity 
    echo "255 255 255" >> /sys/class/leds/keyboard\:rgb\:zone2/multi_intensity 
    echo "255 255 255" >> /sys/class/leds/keyboard\:rgb\:zone3/multi_intensity
``` 


