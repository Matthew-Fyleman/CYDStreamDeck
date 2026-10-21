# CYD Stream Deck

## Role & Persona: 

You are an esp32 programmer creating a simple CYD based stream deck that will use the touchscreen of a CYD to allow the selection and launching of applications and URLs on a connected Windows 11 computer.

## Context: 

The main component is:

An ESP-32 based 4 inch CYD currently attached to COM4 and utilized by the Clocky project described in the repository found in the C:\Users\matth\OneDrive\Documents\PlatformIO\Projects\Clocky folder of this computer. There is no need to preserve the Clocky firmware currently on the device when the firmware for this project is uploaded.

## Main Tasks: 

1. Analyse the code in the Clocky repository to get the necessary information about the CYD attached to COM4, such as display pixel dimensions, ESP32 pinout and connections to the display.
2. Create new firmware for the CYD to provide the functionality to act as an interface to allow the selection and launching of applications and URLs on a connected Windows 11 computer. 
3. Create the necessary supporting daemon to run on the Windows 11 computer to allow the computer to respond to read the selected icon grid position and respond appropriately and promptly. The daemon should have a negligeable impact on the Windows 11 computer's performance.
4. Create a configuration application to run on the Windows 11 computer to allow icons for the applications/urls to be uploaded to the CYD, and to allow the configuration of the actions the Windows 11 computer should take in response to the selection of a particular application/url.

## Detailed specification:

### Firmware

Create firmware for the ESP32 built-in to the CYD that will do the following:

1. On boot after reset, if it finds no stored WiFi credentials, will create a WAP and provide a rudimentary web site for entering the local WiFi credentials through a user-friendly interface. Once the WiFi credentials have been entered and verified, they will be securely stored in NVM on the microcontroller, and the microcontroller will reset.
2. On boot after reset, if it finds WiFi credentials in the NVM it will attach to the WiFi,.
3. The display will be in landscape mode and show a grid of 12 square icons arranged in three rows of four with a small amount of spacing (no more than 5 pixels) between icons horizontally and vertically.
4. There will be a row at the bottom of the display, no more than 20 pixels high, that does not interfere with the icons, that will display the current time (hours and minutes only) and date. The timezone and format will be configurable through the configuration application described below.
5. Clicking anywhere in the time/date row will cause the device’s current IP address to be displayed for 10 seconds.
6. Clicking within the bounds of any icon will cause a grid position for that icon to be transmitted to the daemon running on the Windows 11 computer.
7. The display will also show a visual indication that the icon has been pressed, but this indication will not be sticky, i.e. it will last only long enough for the user to see that the action has been acknowledged and after that the icon will revert to its former state.

### Daemon

Create a daemon in Python to run on the Windows 11 computer that will do the following:

1. Monitor the CYD over a COM port (or if possible the WiFi network it is attached to) to listen for the selected grid position of an icon on the CYD display. 
2. On receiving a grid position from the CYD, read the actions associated with that grid position in the configuration and execute them. This will normally be to launch an executable, with or without parameters, or to open a URL in the default browser on the computer.
3. The daemon will automatically detect the COM port the CYD is connected to, even if there are other CYD devices attached to other COM ports. This is not necessary if two-way communication with the CYD is handled over WiFi.

### Configuration application

Create a configuration application in Python to run on the Windows 11 computer that will allow the user to do the following configuration tasks:

1. Allow image files of an appropriate size (.jpeg, .png or similar format, stored in a configuration folder on the computer) to be associated with, and uploaded to, each grid position on the CYD display. 
2. Allow the specification of a set of actions to be associated with each grid position on the CYD display. These actions will normally be to launch an executable, with or without parameters, or open a URL in the default browser on the computer.
3. Allow the specification of the timezone (including allowing for DST), time format (12 hour or 24 hour) and date format (from a selection of common date formats) for the time/date display line on the CYD.
4. Allow the editing of the WiFi credentials.
5. Provide a button to upload the configuration information to the CYD either via a COM port or WiFi if it is used for two-way communication with the CYD.
6. Store the configuration information in a configuration folder on the computer so the configuration application does not need to be running under normal operation.

## Output: 

Create an FSD for the project in .MD format in the docs folder located in the current folder, then generate the firmware code for the project as a Platform IO project in the current folder. Ensure the firmware compiles correctly and upload to the CYD on COM4. Generate the Python code for the daemon and the configuration application store it in an appropriate folder in the current folder and ensure both compile and run correctly. Place all code and documentation under git control in the repository in the current folder. Commit and push. 