JCP is the main program used for communication with the Skunkboard.

It requires LibUSB 0.1 to be available. For Windows, the .lib and header file are provided. 
For Linux or OSX, you must get these files yourself and set up LibUSB on your system correctly.

http://libusb.sourceforge.net/

The jcp.vcproj and libusb.lib files are used for people building under Windows.
The Makefile is used for people building under Linux or OSX.

No official support is given for user-built binaries, however, if you are stuck you
can always drop by the forums at http://www.harmlesslion.com/phpBB2/viewforum.php?f=12

---

12 Sep 2008 - Added support for ABS binaries
21 Sep 2008 - Added patches to enable OSX to build (Thanks to Belboz)
 4 Oct 2008 - Added word write option for machines with flakey power. Refactored the code to help shell authors with better helper functions.
16 Oct 2008 - Moved word write into the ROM for BIOS 1.1.0, tweaked skunkCONSOLECLOSE to wait before closing, added BIOS upgrade code
21 Oct 2008 - Patch to above release - make file loading more safe, fix flash memory access error

 