Fork notes:  
Display changed from SPI to I2C in defines.h  
Removed RuSSian language, because fuck RuSSia 
The pressed brake flag threshold was set too high and could not enter the menu because of this - fixed  
I used Nano instead of Pro Mini, but i had to cut the RX/TX legs of the CH340, otherwise it didn't work. Nano has 3.3V output, you can power the OLED from there  
If you don't want to install old version of Arduino IDE, download the hex files from releases (no bootloader included). You can burn the hex files to Arduino using Avrdudess and e.g.USBtinyISP. If it's not working, in Avrdudess' options you can set the exe and the conf to the provided files  
A nice 3D printed case here https://www.thingiverse.com/thing:3646680

# Xiaomi M365 Display [This project is no longer maintained, because I don't have a platform to test it.]

I will be pushing pull requests, if you improve this in any way possible feel free to create a push request.

<img src="https://user-images.githubusercontent.com/5514002/56957966-22138500-6b49-11e9-8e42-26d0758b6d00.jpg" width="200" height="264" />

# Products Used  
Arduino Pro Mini    
I2C OLED 0.96" or 1.3" Screen
FTD1232 Usb Programmer   
3d Printed Bracket  
1N4148 Diode  
0.25w 120ohm Resistor       

Estimated price is around 20$ (Inluding Printed Parts).

Knowing the price is around 20$ you can get the Xiaomi M365 Pro top panel from aliexpress for around the same price and a better look, unless you need additional features just go for the pro display.

# Flashing  
![alt text](https://i.imgur.com/DpPkvJz.jpg)  
Please install the libraries I provided in the files, install them to you arduino library folder, usually              
  C:\Users\\%username%\Documents\Arduino\libraries  
I'd recommend you to use Arduino 1.6.6  
https://www.arduino.cc/en/Main/OldSoftwareReleases  

# Physical Connections  

![dashboard](https://github.com/user-attachments/assets/e0b65522-345c-487b-b7f3-a1857144189d)

# Updating M365 firmware / Disabling the Dashboard
Turn on the scooter and immediately engage and hold the throttle and brake before the logo disappears from the dashboard LCD. You will enter on dashboard disabled mode.
The Arduino TX/RX pins will go to hi impedance state leaving the communication BUS free.

By this way you can update de M365 firmware without disconnecting the dashboard or any cable.

A new power cycle will reset the dashboard to normal mode.

# Known Issues  
Sometimes the Arduino Freezes, a watchdog is in place but doesn't always trigger.  

# Screen caps
# Soldering, soldered directly to the cable coming from the MCU
5V To Red    
GND To Black  
BUS To Yellow  
![alt text](https://i.imgur.com/3ZwcrIJ.jpg)  
A video on how everything is soldered may come soon.

Meanwhile you can enable subtitles in English in this YouTube video produced in Spanish language
https://www.youtube.com/watch?v=JQUNXCyj2Fs

# UI
UI pictures from version 0.2  


![alt text](https://i.imgur.com/8ekMdIo.jpg)  
![alt text](https://i.imgur.com/AHLVTcu.jpg)  

More pictures are coming soon.
