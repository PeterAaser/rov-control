# motor_interface
This package is the interface between the ROV control system running on a Raspberry Pi and the [BlueRobotics ESCs](https://www.bluerobotics.com/store/thrusters/besc-30-r1/). The interfacing is tested on a [Adafruit PCA9685](https://www.adafruit.com/product/815) 16-channel 12-bit PWM board, but will probably work on other PCA9685 boards as well.

## Usage
`roslaunch motor_interface motor_interface.launch` to run this package as a standalone. In normal use it should be run from a launch script that also starts all the other modules of the system.

## Dependencies
* The [Adafruit Python PCA9685](https://github.com/adafruit/Adafruit_Python_PCA9685) driver
`sudo pip install adafruit-pca9685`
* The [NumPy](http://www.numpy.org/) Python library
`sudo apt install python-numpy`
* Working [I2C on the Raspberry Pi](https://www.raspberrypi.org/forums/viewtopic.php?f=44&t=104133)

## Notes
The node will only run if the PCA9685 is properly connected to the Raspberry Pi (or other host computer). Connect VCC to a 3.3 V pin, SCL to SCL, SDA to SDA, and ground to ground.
