# Working LIRC port for Mac OS X

This is a working and tested LIRC port for Mac OS X . 

### Features:

- Working driver for the Irdroid USB Infrared Transceiver / Irtoy
- Working portaudio driver for using the sound card to generate output signals

### Version
0.9.3 git
### Grab a sample of the USB IR Transceiver below

You can grab a sample board from http://www.irdroid.com/irdroid-usb-ir-transceiver/

### Tech

The Irdroid USB Infrared Transceiver can be used in Windows, Linux , MAC OSX and Android. We provide source code examples for the above platforms . The modules is based on the USB IRToy from Dangerous Prototypes.

### Todos

 - N/A

License
----
GPL 2.0

### MISC // portaudio and LIRC on Mac OS X

To compile the portaudio driver in MAC OS X you need to pass the following to the command line
1. Download and install portaudio library - needed to build the LIRC port Audio driver
2. Pass the following in the console
3. run ./configure

export CPPFLAGS='-I/usr/local/include'
export LDFLAGS='-L/usr/local/lib/'
