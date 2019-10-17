/* jcp.cpp : Move data to Jaguar by way of EZ-HOST RDB connection 
   Skunkboard support - http://harmlesslion.com

   Design:
   Data is copied at peak speed via RDB into two buffers in EZ memory (at $2800 and $1800)
   The 68K polls EZ memory for new buffers, copying them into Jaguar memory as they appear

   Memory map relative to base address $2800:
   $37E0:	Base address (in 68K memory) for this block
   $37E4:	Address where 68K execution begins once this block is complete
   -1 if more blocks will follow (i.e., stay in the reader loop)
   $37E8:	Base address of next block in EZ-HOST (usually $1800)
   $37EA:	Amount (in bytes) to copy (up to 4064 bytes)
   -1 if the block is not ready for copying
   The 68K sets this at $2800 and $1800 before transfer begins
   After each block is moved, the 68K sets this to -1 again. Some
   sequences are bi-directional.

   Note:   the values FxFF, excluding FFFF, are reserved as flag values
   for future, incompatible versions of JCP, to allow detection.

   Observations:
   The X command seems to have a limit of 4080 bytes per transfer
   This applies even with escaping!  (Which makes 9120 bytes...)
   Roundtrips hurt -- compare 10 seconds/megabyte @ 4080 to 13 seconds/megabyte @ 2048
   We currently use 'middle endian' because the CPLD does not byteswap 'data regions'

   Lots and lots of tweaks by Tursi, sorry, not all documented, though I've updated what
   I changed above.

   This program and associated binaries are copyright by Mike Brent, http://harmlesslion.com
   Commercial use prohibited. All rights reserved. Copyright 2008 by Mike Brent.

   linux link with -lusb and -lrt - requires libusb to be installed!
   Apple link with only -lusb

   May require root priviledges to see the device.

   Thanks to Belboz for the OSX patch!

*/

/* if you don't want to include the BIOS upgrade, comment this out */
#define INCLUDE_BIOS_101
/* uncomment this to try automatic mode (experimental) */
//#define JCP_AUTO

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __APPLE__
#include <malloc.h>
#endif
#include <assert.h>
#include <errno.h>
#include <time.h>
#ifdef WIN32
#include "winusb.h"
#else
#include <usb.h>
#endif
#include "turbow.h"
#include "univbin.h"
#include "romdump.h"
#include "flashstub.h"
#include "dumpver.h"
#include "flash_cof.h"
#ifdef INCLUDE_BIOS_101
#include "upgrade101.h"
#endif

#include <sys/time.h>
#include <ctype.h>

#include "jcp_handler.h"

// version major.minor.rev
#define JCPVERSION 0x010301
// ROM based address that we can blindly send dummy data to
#define DUMMYBASE 0xFFE000

#define	ENBIGEND(_x) (((_x)[0] << 24) + ((_x)[1] << 16) + ((_x)[2] << 8) + (_x)[3])
#define	HALFBIGEND(_x) (((_x)[0] << 8) + (_x)[1])
#define	ENMIDEND(_x) (((_x)[0] << 16) + ((_x)[1] << 24) + ((_x)[2]) + ((_x)[3] << 8) )
#define	HALFLITTLEEND(_x) (((_x)[0]) + ((_x)[1] << 8) )

#define uchar unsigned char
#ifndef bool
#define bool int
#endif
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif

#ifndef WIN32
/* linux compatibility with Windows terms */
#define DWORD unsigned int
#define _stricmp strcasecmp
#define Sleep(x) usleep(x*1000)

/* returns a count in ms - this one should be osx compat */
DWORD GetTickCount() {
  struct timeval now;
  DWORD ret;

  int rv = gettimeofday(&now, NULL);

  if (rv != 0) {
    return 0;	/* we got nothing */
  }

  ret=now.tv_sec*1000+(now.tv_usec/1000);
  return ret;
}
#endif

usb_dev_handle* findEZ(bool fInstallTurboW, bool fAbortOnFail);
void bye(char* msg);
void SendFile(int flen, uchar *fptr, int curbase, int base);
void DoFile(uchar *fdata, int base, int flen, int skip);
void LockBothBuffers();
bool TestIfBuffersLocked();
void WaitForBothBuffers();
void DoResetAndReconnect(bool bForce);
void DoResetAndBoot();
void DoFlash(int nLen);
void DoDump(char *pszName);
void DoSerialInfo();
void DoBiosUpdate();
void DoReset();
void HandleConsole();
void FilenameSanitize(char *buf);

/* globals */
int nextez = 0x1800;
usb_dev_handle* udev = NULL;
uchar *fdata = NULL;
char g_szFilename[256];
FILE *fp=NULL;
bool g_FirstFileSent=false;

bool g_OptDoFlash=false;
bool g_OptDoSlowFlash=false;
bool g_OptOverrideFlash=false;
bool g_OptEraseAllBlocks=false;
bool g_OptDoDump=false;
bool g_OptFlashActive=false;
bool g_OptNoBoot=false;
bool g_OptOnlyBoot=false;
bool g_OptOverride=false;
bool g_OptConsole=false;
bool g_OptOnlyConsole=false;
bool g_OptSilentConsole=false;
bool g_OptVerbose=false;
bool g_OptAutoMode=false;   /* used to enable JCP auto mode - this is experimental */

/* Main function - entry point */
int main(int argc, char* argv[])
{
  int base, flen, skip;
  int nFileArg, nBaseArg, nArg;

  printf("jcp v%02X.%02X.%02X built on %s\n\n", ((JCPVERSION&0xFF0000)>>16), ((JCPVERSION&0xFF00)>>8), (JCPVERSION&0xFF), __DATE__);

  if ((argc<2) || ((argc>1)&&(strchr(argv[1],'?')))) {
    bye("Usage: jcp [-rewfnbocdsu] file [$base]\n"			\
	"  -r = reset (no other args needed)\n"			\
	"  -f = flash (pass filename + opt base)\n"		\
	"  -wf= word flash (slow, only if 'f' alone fails)\n"	\
	"  -ef= erase whole flash\n"		\
	"  -n = no boot (pass filename + opt base)\n"	\
	"  -b = boot address (pass base only)\n"		\
	"  -o = override address (pass filename and base)\n" \
	"  -c = launch console (incompatible with n)\n"	\
	"  -d = dump flash (pass filename)\n" \
	"  -s = get board version and serial number\n" \
	"  -u = upgrade board bios (if available)\n" \
	""
	);
  }

  // Some basic initialization
  fdata = (uchar*)malloc(4200000);	// 4MB + header
  memset(fdata, 0, 4200000);
  base = 0x4000;
  flen = 0;
  skip = 0;	
  strcpy(g_szFilename, "");
  nFileArg=1;
  nBaseArg=2;

#ifdef JCP_AUTO
  g_OptAutoMode=true;
#endif

  nArg=1;
  while (argv[nArg][0] == '-') {
    int nPos;

    nFileArg++;
    nBaseArg++;
    nPos=1;
    while (argv[nArg][nPos]) {
      switch (tolower(argv[nArg][nPos])) {
      case 'v': g_OptVerbose=true; break;
      case 'f': g_OptDoFlash=true; base=0x802000; break;
      case 'w': g_OptDoSlowFlash=true; break;		// 'w'ord writes
      case 'e': g_OptEraseAllBlocks=true; break;
      case 'd': g_OptDoDump=true; break; 
      case 'r': DoReset(); bye(""); break;
      case 'n': g_OptNoBoot=true;	break;
      case 'b': g_OptOnlyBoot=true; break;
      case 'o': g_OptOverride=true; break;
      case 'c': g_OptConsole=true; break;
      case 's': DoSerialInfo(); Sleep(100); DoReset(); bye(""); break;
      case 'u': DoBiosUpdate(); Sleep(100); DoReset(); bye(""); break;
      case '!': g_OptOverrideFlash=true; break;	// undocumented!
      default: bye("Unknown option");
      }
      nPos++;
    }
    nArg++;
    if (nArg >= argc) {
      break;
    }
  }

  if ((argc > nBaseArg) && (argv[nBaseArg][0] == '$')) {
    if (1 != sscanf(&argv[nBaseArg][1], "%x", &base)) {
      bye("Failed to parse base address");
    }
  } else if (g_OptOverride) {
    bye("Override option must specify both filename and base address");
  }

  if ((g_OptOnlyBoot) && (argc > nFileArg) && (argv[nFileArg][0] == '$')) {
    if (1 != sscanf(&argv[nFileArg][1], "%x", &base)) {
      bye("Failed to parse base address");
    }
  } 

  if (!g_OptOnlyBoot) {
    if ((nFileArg >= argc) && (!g_OptConsole)) {
      bye("No filename was specified");
    } else {
      if (nFileArg >= argc) {
	// user seems to want to try to attach the console, so don't try to load stuff
	if (!g_OptOnlyBoot) {
	  g_OptOnlyConsole=true;
	}
      } else {
	strncpy(g_szFilename, argv[nFileArg], 256);
	g_szFilename[255]='\0';
	if (!g_OptDoDump) {
	  FILE *fp = fopen(g_szFilename, "rb");
	  if (NULL == fp || (flen = (int)fread(fdata, 1, 4200000, fp)) < 1) {
	    bye("Couldn't read file");
	  }
	  fclose(fp);
	} else {
	  flen=0;
	}
      }
    }
  }

  if (g_OptDoFlash) {
    if (flen == 0) {
      bye("File must be specified with flash!");
    }
    if (g_OptNoBoot) {
      printf("- NoBoot option not supported during flashing\n");
      g_OptNoBoot=false;
    }
    DoFlash(flen);
    g_OptFlashActive=true;
  }

  if (g_OptDoDump) {
    DoDump(g_szFilename);
    Sleep(100); 
    DoReset(); 
    bye("Dump complete.");
  }

  DoFile(fdata, base, flen, skip);

  free(fdata);
  fdata=NULL;
  if (NULL != udev) {
    usb_close(udev);
  }
}

/* Generate a little text spinner */
void Spin() {
  static int nSpinner=0;
  static char szSpin[]="\\|/-";

  nSpinner++;
  if (szSpin[nSpinner] == '\0') nSpinner=0;
  putchar(szSpin[nSpinner]);
  putchar('\b');
  fflush(stdout);
}

/* reset the Jaguar then reconnect to it - pass true not to wait on the buffers */
void DoResetAndReconnect(bool bForce) {
  if (!bForce) {
    WaitForBothBuffers();	// make sure the Jag is done the last command
  }

  LockBothBuffers();		// carries through the reset

  DoReset();
  Sleep(2000);			// takes the Jag about 2s to come up

  while (NULL == udev) {
    Sleep(100);
    udev=findEZ(true, false);
  }

  WaitForBothBuffers();	// when the Jag clears the buffers, we're up

  // reset pointer
  nextez = 0x1800;
}

/* synch reset the Jag, then send a start packet for the cart */
void DoResetAndBoot() {
  DWORD tmp=0;

  DoResetAndReconnect(false);

  // send the fake boot command
  g_OptOnlyBoot=true;
  g_OptFlashActive=false;

  DoFile((uchar*)&tmp, 0x802000, 0, 0);
}

/* mark both buffers as blocked - don't use during upload! */
void LockBothBuffers() {
  unsigned short tmp;

  tmp=0;
  if (usb_control_msg(udev, 0x40, 0xfe, 4080, 0x1800+0xFEA, 
		      (char*)&tmp, 2, 1000) != 2) {
    assert(0);
    printf("Failed to send block fully (control-c to abort)\n");
    Sleep(500);
  }
  if (usb_control_msg(udev, 0x40, 0xfe, 4080, 0x2800+0xFEA, 
		      (char*)&tmp, 2, 1000) != 2) {
    assert(0);
    printf("Failed to send block fully (control-c to abort)\n");
    Sleep(500);
  }
}

/* returns true if both buffers are locked */
bool TestIfBuffersLocked() {
  volatile short poll=0;
  bool bRet=false;

  if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x1800+0xFEA, (char*)&poll, 2, 1000) == 2)) {
    if (poll == 0) {
      if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800+0xFEA, (char*)&poll, 2, 1000) == 2)) {
	if (poll == 0) {
	  bRet=true;
	}
      }
    }
  }

  return bRet;
}

/* wait for the Jag to mark both buffers as free */
void WaitForBothBuffers() {
  volatile short poll=0;

  // first buffer
  do {
    Spin();
    if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x1800+0xFEA, 
			 (char*)&poll, 2, 1000) != 2)) {
      Sleep(500);
    } else {
      Sleep(100);
    }
  } while (-1 != poll);
  printf(".");
	
  // second buffer
  do {
    Spin();
    if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800+0xFEA, 
			 (char*)&poll, 2, 1000) != 2)) {
      Sleep(500);
    } else {
      Sleep(100);
    }
  } while (-1 != poll);
  printf(".\n");
}

/* Reset the Jaguar */
void DoReset() {
  // We have to use scan mode to access registers (TurboWrite uses DMA engine)
  unsigned char cmd[10] = {0xB6, 0xC3, 0x04, 0x00, 0x00, 0x28, 0xC0, 0x02, 0x00, 0x00};

  if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
    printf("Resetting jaguar...\n");
  }

  // Reset is 0xc028=2, 0xc028=0
  if (NULL == udev) {
    udev = findEZ(false, true);		// don't load turbow
  }

  if (usb_control_msg(udev, 0x40, 0xff, 10, 0x304C, (char*)cmd, 10, 1000) != 10) {
    bye("Reset assert failed to send.");
  }

  // brief delay!
  Sleep(50);

  cmd[7] = 0;
  if (usb_control_msg(udev, 0x40, 0xff, 10, 0x304C, (char*)cmd, 10, 1000) != 10) {
    bye("Reset release failed to send.");
  }

  /* in case it's used elsewhere */
  if (NULL != udev) {
    usb_close(udev);
  }
  udev=NULL;
}

/* Prepare the Jaguar to receive a flash file */
void DoFlash(int nLen) {
  volatile short poll=0;
  int idx;
  unsigned int nBlocks;

  g_OptSilentConsole=true;

  // due to the flash layout, we can't do a straight sector erase
  // we compromise some and erase only half if we don't need it
  // all - that seems to work okay.
  if (nLen <= 2*1024*1024) {
    nBlocks = 32;
  } else {
    nBlocks = 62;
  }
  // just in case it's needed
  if (g_OptEraseAllBlocks) {
    nBlocks=62;
  }

  // restrict to the legal range
  if (nBlocks > 62) nBlocks=62;
  if (nBlocks < 1) nBlocks=1;

  if (g_OptVerbose) {
    printf("Going to erase %d blocks\n", nBlocks);
  }

  if (g_OptDoSlowFlash) {
    printf("Using slow flash (experimental...)\n");
    nBlocks|=0x80000000;	// set high bit to flag it
  }

  // FLASHSTUB needs to be non-const for this to work
  for (idx=0; idx<SIZE_OF_FLASHSTUB; idx++) {
    if ((FLASHSTUB[idx]==0x0a) && (FLASHSTUB[idx+1]==0xbc) && (FLASHSTUB[idx+2]==0xde) && (FLASHSTUB[idx+3]==0xf0)) {
      FLASHSTUB[idx]=(nBlocks&0xff000000)>>24;
      FLASHSTUB[idx+1]=(nBlocks&0xff0000)>>16;
      FLASHSTUB[idx+2]=(nBlocks&0xff00)>>8;
      FLASHSTUB[idx+3]=(nBlocks&0xff);
      break;
    }
  }
  if (idx>=SIZE_OF_FLASHSTUB) {
    bye("Failed to find signature - internal error.");
  }

  DoFile((uchar*)FLASHSTUB, 0x4100, SIZE_OF_FLASHSTUB, 0);

  // Don't scan for the buffers to be ready till they are zeroed, indicates start of flash
  // first buffer
  do {
    Spin();
    if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x1800+0xFEA, 
			 (char*)&poll, 2, 1000) != 2)) {
      Sleep(500);
    } else {
      Sleep(100);
    }
  } while (0 != poll);
  printf(".");
	
  // second buffer
  do {
    Spin();
    if ((usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800+0xFEA, 
			 (char*)&poll, 2, 1000) != 2)) {
      Sleep(500);
    } else {
      Sleep(100);
    }
  } while (0 != poll);
  printf(".\n");

  // blocks are 64k each, and each takes about 300ms (somewhat less) to erase
  printf("Waiting for erase to complete (about %ds)", ((nBlocks+1)*300)/1000);

  // Both buffers will be marked ready when the
  // Jag is ready to proceed.

  WaitForBothBuffers();

  g_OptSilentConsole=false;

  // reset pointer
  nextez = 0x1800;
}

/* Request the Jaguar to dump the flash (uses the console to collect it) */
void DoDump(char *pszName) {
  int nCnt, nRes;
  DWORD nEnd,nStart=GetTickCount();

  // First we need to open the file and write the universal header to it
  fp=fopen(pszName, "wb");
  if (NULL == fp) {
    bye("Can't open output file!");
  }
  nCnt=(int)fwrite(univbin, 1, sizeof(univbin), fp);
  // binary doesn't include the useless 0xff padding to 8k, so do that here
  // except, there is a tiny block at 0x400 we need to fill in with standard
  // values.
  while (nCnt < 0x400) {
    fputc(0xff, fp);
    nCnt++;
  }
  // standard values - 32-bit cart, start at 0x802000, show logo
  fputc(0x04, fp);
  fputc(0x04, fp);
  fputc(0x04, fp);
  fputc(0x04, fp);
  fputc(0x00, fp);
  fputc(0x80, fp);
  fputc(0x20, fp);
  fputc(0x00, fp);
  fputc(0x00, fp);
  fputc(0x00, fp);
  fputc(0x00, fp);
  fputc(0x00, fp);
  nCnt+=12;

  // now the rest of the padding
  while (nCnt < 8192) {
    fputc(0xff, fp);
    nCnt++;
  }

  // This will make DoFile shell out to the console before returning
  g_OptConsole=true;
  g_OptSilentConsole=true;

  printf("Beginning dump to '%s'...\n", pszName);
  DoFile((uchar*)ROMDUMP, 0x10000, SIZE_OF_ROMDUMP, 0);

  nEnd=GetTickCount();
  nRes=(nEnd-nStart)/1000;
  if (nRes > 0) {
    printf(" \nDumped 4MB in %ds - %dKB/s\n", nRes, (4*1024-8)/nRes);
  } else {
    printf("Dump time <1s\n");
  }
}

/* Request the Jaguar to print out serial number (uses the console to collect it) */
void DoSerialInfo() {
  // This will make DoFile shell out to the console before returning
  g_OptConsole=true;
  g_OptSilentConsole=true;
  DoFile((uchar*)DUMPVER, 0x5000, SIZE_OF_DUMPVER, 0);
}

/* Do a BIOS update (if available) */
void DoBiosUpdate() {
#ifdef INCLUDE_BIOS_101
  g_OptConsole=true;
  g_OptSilentConsole=true;
  DoFile((uchar*)Upgrade101, 0x80000, SIZE_OF_UPGRADE101, 0);
#else
  bye("BIOS update not included in this build of JCP");
#endif
}

/* Writes a block to the Jaguar */
/* uchar points to data to write, curbase is the base to load at, 
   start is the start address or -1 if not starting yet, and len
   is the number of bytes to load. len should be even or at least
   the buffer must be an even size!
   This function writes into the other-than-current block */
void WriteABlock(uchar *data, int curbase, int start, int len) {
  uchar block[4080];
  int i;
  volatile unsigned short poll;

  if ( ((curbase >= 0x800000) && (curbase < 0x802000)) ||
       ((curbase+len >= 0x800000) && (curbase+len < 0x802000)) ) {
    printf("\n* Skipping block at 0x%08X - unwritable.\n", curbase);
    return;
  }
  if ((g_OptDoFlash) && (g_OptFlashActive)) {
    if (curbase < 0x802000) {
      printf("\n* Skipping block at 0x%08X - not flash!\n", curbase);
      return;
    }
  }

  memset(block, 0, 4080);

  // 'Fix' the byte order for the next block of file data
  for (i = 0; i < len; i += 2) {
    block[i+1] = *data++;
    block[i] = *data++;
  }

  // Set up block trailer
  block[0xFE2] = curbase & 255;
  block[0xFE3] = (curbase >> 8) & 255;
  block[0xFE0] = (curbase >> 16) & 255;
  block[0xFE1] = (curbase >> 24) & 255;

  block[0xFE6] = start & 255;
  block[0xFE7] = (start >> 8) & 255;
  block[0xFE4] = (start >> 16) & 255;
  block[0xFE5] = (start >> 24) & 255;

  block[0xFE8] = 0;
  block[0xFE9] = nextez >> 8;
  nextez = (0x1800 == nextez) ? 0x2800 : 0x1800;

  block[0xFEA] = len & 255;
  block[0xFEB] = (len >> 8) & 255;

  if (g_OptVerbose) {
    printf("ez: %04x  start: %08x  len: %04x  base: %08x/%08x\n", nextez,
	   ENMIDEND(block+0xfe4), HALFLITTLEEND(block+0xfea), ENMIDEND(block+0xfe0), curbase);
  }

  // Wait for the block to come free (handshake with 68K).
  poll=0;
  do {
    Spin();
 
    if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA, 
			 (char*)&poll, 2, 1000) != 2)) {
      assert(0);
      printf("Failed to handshake with 68k (control-c to abort)\n");
      Sleep(1000);
    }
    //			printf("Polled 0x%08x\n", poll);
    // poll is unsigned. But a value over 0xf0xx is reserved for future use
    // and we can't count on the lower bytes to be correct since the high
    // byte can change first in rare race conditions.
  } while (0xf0ff != (poll&0xf0ff));

  // any value except 0xffff indicates a future use, possibly that
  // this is a new firmware. We only need this here because this is
  // always the first block sent to the Jaguar.
  if (poll != 0xffff) {
    if (g_OptVerbose) {
      printf("value %04X", poll);
    }
    bye("Got invalid value from block synchronization. Please use a newer JCP.");
  }

  // Send off the finished block.
  if (usb_control_msg(udev, 0x40, 0xfe, 4080, nextez, 
		      (char*)block, 4080, 1000) != 4080) {
    assert(0);
    printf("Failed to send block fully (control-c to abort)\n");
    Sleep(500);
  }

  // check for successful start
  if (-1 != start) {
    // since this block should have triggered a boot on Jag, we wait for it
    // Jag should set to 0000 or 8888 (with a possible ffff intermediate)
    // Wait for the block to change to a valid setting (handshake with 68K).
    poll=0;
    do {
      Spin();
	 
      if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA, 
			   (char*)&poll, 2, 1000) != 2)) {
	assert(0);
	printf("Failed to handshake with 68k (control-c to abort)\n");
	Sleep(1000);
      }
      //			printf("Polled 0x%08x\n", poll);
    } while ((0x0000 != poll) && (0x8888 != poll));

    if (poll == 0x8888) {
      bye("Unauthorized. You must flash a different rom to proceed.\n(Remember to reset the jag with 'jcp -r'!)");
    } else {
      if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
	printf("Jag accepted start request at $%08X.\n", start);
      }
    }
  }
}

/* Send a file to the Jaguar */
void SendFile(int flen, uchar *fptr, int curbase, int base) {
  int dotty=0;
  int len;

  while (flen > 0) {
    int start = (flen <= 4064) ? base : -1;

    if (g_OptNoBoot) {
      start=-1;
    } else {
      if (-1 != start) {
	dotty=0;
	if (g_OptVerbose) {
	  printf(" \nBooting address $%X\n", start);
	} else {
	  printf(" \n");
	}
      }
    }

    len = (flen <= 4064) ? flen : 4064;

    WriteABlock(fptr, curbase, start, len);

    fptr += 4064;
    curbase += 4064;
    flen -= 4064;

    dotty = (dotty + 1) & 7;
    if (0 == dotty) {
      putchar('.');
    }
  }

  // if this is a no-boot case, we need to make sure the next block will
  // be at $2800, as that's the only address jcp polls to start up. So
  // if needed, we'll send a little dummy block here, like with -b
  if ((g_OptNoBoot) && (nextez != 0x1800)) {
    DWORD dummy=0;
    WriteABlock((unsigned char*)&dummy, DUMMYBASE, -1, 4);
  }
}

/* Parse a file for headers, and prepare to send it to the Jaguar */
void DoFile(uchar *fdata, int base, int flen, int skip) {
  // Send the data to the Jaguar, 4064 bytes at a time
  int curbase, dotty=0;
  int ticks, oldlen;
  uchar *fptr;

  if (!g_OptOnlyBoot) {
    // Check file header
    if ((flen > 0x2000) && (0x802000 == ENBIGEND(fdata+0x404))) {
      if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
	printf("Cart ROM:  ");
      }
      if (!g_OptOverride) base = 0x802000;
      skip = 0x2000;
    } else if ((flen > 0x2200) && (0x802000 == ENBIGEND(fdata+0x604))) {
      if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
	printf("Cart ROM + 512:  ");
      }
      if (!g_OptOverride) base = 0x802000;
      skip = 0x2200;
    } else if ((flen > 72) && (fdata[0] == 0x01) && (fdata[1] == 0x50)) {
      if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
	printf("COFF File:  ");
      }
      if (!g_OptOverride) base = ENBIGEND(fdata+56);
      skip = ENBIGEND(fdata+68);
      if (flen <= skip) {
	bye("Detection error or corrupt file.\n");
      }
    } else if ((flen > 0x30) && (fdata[0] == 0x7f) && (fdata[1] == 'E') && (fdata[2] == 'L') && (fdata[3] == 'F')) {
      int loadbase;
      int secs, seclen;
      uchar *img, *secptr;

      if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
	printf("ELF File:  ");
      }

      if ((fdata[5] != 0x2) || (0x20004 != ENBIGEND(fdata+0x10))) {
	bye("Not 68K executable.");
      }
      skip = loadbase = ENBIGEND(fdata+0x18);
      flen = 0;
		
      // Map all the sections into a new memory image. Not necessarily entirely safe.
      secs = HALFBIGEND(fdata+0x30);
      seclen = HALFBIGEND(fdata+0x2e);
      img = (uchar*)malloc(2048*1024);
      secptr = fdata+ENBIGEND(fdata+0x20);
      memset(img, 0, 2048*1024);
      while (secs-- >= 0) {
	int sadr = ENBIGEND(secptr+0xc), slen = ENBIGEND(secptr+0x14);
	uchar* fptr = fdata+ENBIGEND(secptr+0x10);
	if (0 != sadr) {		// 0 is debug info, so ignore it
	  if (sadr < loadbase)
	    bye("Section has base address below entry point.  See readelf for details.");
	  if (sadr+slen > flen)
	    flen = sadr+slen;
	  if (flen >= 2048*1024 || loadbase < 0)
	    bye("Section falls outside Jaguar memory.  See readelf for details.");
	  if (1 == ENBIGEND(secptr+0x4))	// Progbits, so copy them
	    memcpy(img+sadr, fptr, slen);
	}
	secptr+=seclen;
      }
      free(fdata);	// Point to the newly created memory image
      fdata = img;
      if (!g_OptOverride) base=loadbase;
    } else if ((flen > 0x2e) && (fdata[0x1c] == 'J') && (fdata[0x1d] == 'A') && (fdata[0x1e] == 'G') && (fdata[0x1f] == 'R')) {
      if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
	printf("Jag Server Exe: ");
      }
      if (!g_OptOverride) base=ENBIGEND(fdata+0x22);
      skip=0x2e;
    } else if ((flen > 0x24) && (fdata[0] == 0x60) && (fdata[1] == 0x1b)) {
      printf("DRI ABS File:  ");
      skip = 0x24;
      base = ENBIGEND(fdata+0x16);
      flen = ENBIGEND(fdata+0x6) + ENBIGEND(fdata+0x2) + skip;
    } else if ((flen > 0xa8) && (fdata[0] == 0x01) && (fdata[1] == 0x50)) {
      printf("Alcyon ABS File:  ");
      skip = 0xa8;
      base = ENBIGEND(fdata + 0x28);		// Right now, the code below assumes base = run address.
      // run = ENBIGEND(fdata + 0x24);	// But these files can have different run and base addresses.
      flen = ENBIGEND(fdata+0x18) + ENBIGEND(fdata+0x1c) + skip;
    } else {
      // if all else failed, and the extension is .ROM, assume $802000 load address 
      char *pTmp=strrchr(g_szFilename, '.');
      if (NULL != pTmp) {
	if (_stricmp(pTmp, ".rom")==0) {
	  if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
	    printf("Headerless ROM: ");
	  }
	  if (!g_OptOverride) base=0x802000;
	  skip=0;
	}
      }
    }

    curbase=base;
    flen -= skip;
    if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
      printf("Skip %d bytes, base addr is $%X, length is %d bytes\n", skip, base, flen);
    }

    if ((base >= 0x800000) || (base+flen >= 0x800000)) {
      if ((!g_OptDoFlash) && (!g_OptOverrideFlash)) {
	if (!g_OptAutoMode) {
	  bye("This upload requires the -f option to prepare the flash!");
	}
	/* if the exe was renamed, then we WILL do flash */
	g_OptDoFlash=true;
	DoFlash(flen+skip);
	g_OptFlashActive=true;
      }
    }
  } else {
    // the only-boot mode, we send a dummy block to the top of unused ROM space and then boot the address
    curbase=DUMMYBASE;
    flen=4;		// smallest transfer size
    skip=0;
  }

  fptr = fdata + skip;

  // Open socket to Jaguar
  if (NULL == udev) {
    udev = findEZ(true, true);
  }

  /* if this is the first file, and we are in auto mode, check if reset is needed */
  if ((g_OptAutoMode) && (!g_FirstFileSent)) {
    if (TestIfBuffersLocked()) {
      // buffers locked - probably from a previous upload, so reset
      DoResetAndReconnect(true);
    }
  }

  g_FirstFileSent=true;
  ticks = GetTickCount();
  oldlen=flen;

  if (!g_OptOnlyConsole) {
    printf("Sending...");
    SendFile(flen, fptr, curbase, base);
  }

  if (!g_OptOnlyBoot) {
    int res;

    ticks = GetTickCount() - ticks;
    if (ticks > 0) {
      res=oldlen/ticks;
    } else {
      res=0;
    }
    if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
      printf(" \nFinished in %d millis, %dKB/second.\n", ticks, res);
    }
  }

  if (g_OptConsole) {
    HandleConsole();
  }
}

// Abort nicely(?)
void bye(char* msg) {
  if (msg[0]!='\0') {
    printf("* %s\n", msg);
  }
  if (NULL != udev) {
    usb_close(udev);
  }
  if (NULL != fdata) {
    free(fdata);
  }
  exit(1);
}

/* Locate the Jaguar on the USB bus, open it, get a handle, and upload the turboW tool */
usb_dev_handle* findEZ(bool fInstallTurbo, bool fAbortOnFail) {	
  struct usb_bus *bus;
  struct usb_device *dev = NULL;
	
  usb_init();
  usb_set_debug(0);

  usb_find_busses();
  usb_find_devices();

  for (bus = usb_get_busses(); bus; bus = bus->next) {
    for (dev = bus->devices; dev; dev = dev->next) {
      if (0x4b4 == dev->descriptor.idVendor && 0x7200 == dev->descriptor.idProduct) {
	usb_dev_handle* udev = usb_open(dev);
	if (!udev)	bye("Found, but can't open, EZ-HOST.  In use?\n");
	if (fInstallTurbo) {
	  // load turbow from array
	  int ret = usb_control_msg(udev, 0x40, 0xff, 0, 0x304c, (char*)turbow, SIZE_OF_TURBOW, 1000);

	  if (ret < 1) {
	    printf("Failed to install turbow.bin!\n");
	  } else {
	    if (g_OptVerbose) {
	      printf("Installed turbow.bin: %d scan codes sent\n", ret);
	    }
	  }
	}

	return udev;
      }
    }
  }

  if (fAbortOnFail) {
    bye("Can't find EZ-HOST.\n");	
    // does not return
  }
  return NULL;
}

/* Does all the console functions */
void HandleConsole() {	
  int nTotalFileLength=0;		// bytes written
  uchar block[4080];
  unsigned short tmp;
  int i, len;

  // our basic job is to relay text from the Jaguar. Extension commands are marked with 0xffff as the first two bytes
  // In text mode, we accept text from either buffer, so we scan them both
  if ( (g_OptVerbose) || (!g_OptSilentConsole) ) {
    printf(" \nStarting console...\n");
  }
  if (g_OptDoDump) {
    printf(" \nReceiving flash...\n");
  }

  // blank both buffers
  memset(block, 0, 4080);

  // Set up block trailer
  block[0xFE8] = 0;
  block[0xFE9] = nextez >> 8;
  nextez = (0x1800 == nextez) ? 0x2800 : 0x1800;

  block[0xFEA] = 0xff;
  block[0xFEB] = 0xff;

  // to handshake with the jaguar, we clear the blocks from this end
  // that way the Jag knows we're up and ready.
  tmp=0xffff;
  if (usb_control_msg(udev, 0x40, 0xfe, 4080, 0x1800+0xFEA, 
		      (char*)&tmp, 2, 1000) != 2) {
    assert(0);
    printf("Failed to send block fully (control-c to abort)\n");
    Sleep(500);
  }
  if (usb_control_msg(udev, 0x40, 0xfe, 4080, 0x2800+0xFEA, 
		      (char*)&tmp, 2, 1000) != 2) {
    assert(0);
    printf("Failed to send block fully (control-c to abort)\n");
    Sleep(500);
  }

  // now we can start the main loop
  for (;;) {
    // Wait for the block to be used (handshake with 68K).
    volatile short poll=0;
    do {
      nextez = (0x1800 == nextez) ? 0x2800 : 0x1800;
      // It's actually faster to check this small block and
      // do two reads than to read the whole block just to test
      if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA, 
			   (char*)&poll, 2, 1000) != 2)) {
	assert(0);
	printf("Failed to handshake with 68k (control-c to abort)\n");
	Sleep(1000);
      }
    } while (-1 == poll);

    // Read in the finished block.
    if (usb_control_msg(udev, 0xC0, 0xff, 4080, nextez, 
			(char*)block, 4080, 1000) != 4080) {
      assert(0);
      printf("Failed to read block fully (control-c to abort)\n");
      Sleep(500);
    }

    // acknowledge the buffer as read to delag the jag
    tmp=0xffff;
    if (usb_control_msg(udev, 0x40, 0xfe, 4080, nextez+0xFEA, 
			(char*)&tmp, 2, 1000) != 2) {
      assert(0);
      printf("Failed to send block fully (control-c to abort)\n");
      Sleep(500);
    }

    // deswap the block (including the header)
    for (i=0; i<4080; i+=2) {
      int x=block[i+1];
      block[i+1]=block[i];
      block[i]=x;
    }

    if (g_OptVerbose) {
      printf("Read block from %x, len %d, first bytes: %02x %02x %02x %02x\n", nextez, ((block[0xfea]<<8)|block[0xfeb]), block[0], block[1], block[2], block[3]);
    }
		
    if (0 == ((block[0xfea]<<8)|block[0xfeb])) {
      // bad block (left over flag from booting), ignore
      continue;
    }

    // Now do something with it
    if ((block[0]==0xff) && (block[1]==0xff)) {
      // escape command (16 bit command)
      switch ((block[2]<<8)|block[3]) {
	/* 				case 0:		// nop - can be handy for synchronizing? */
	/* 					if (g_OptVerbose) { */
	/* 						printf("NOP received.\n"); */
	/* 					} */
	/* 					break; */

	/* 				case 1:		// terminate console */
	/* 					if ( (g_OptVerbose) || (!g_OptSilentConsole) ) { */
	/* 						printf("Console terminating.\n"); */
	/* 					} */
	/* 					return; */

	/* 				case 2:		// receive input */
	/* 					{ */
	/* 						// get input from the user */
	/* 						char buf[4064]; */
	/* 						printf("> "); */
	/* 						fgets(buf, 4064, stdin); */
	/* 						buf[4063]='\0'; */

	/* 						// strip EOL */
	/* 						i=(int)strlen(buf)-1; */
	/* 						while (i > 0) { */
	/* 							if (buf[i] < ' ') { */
	/* 								buf[i]='\0'; */
	/* 								i--; */
	/* 							} else { */
	/* 								break; */
	/* 							} */
	/* 						} */

	/* 						// write that input to the jag in the alternate buffer */
	/* 						WriteABlock((unsigned char*)buf, 0, -1, (int)strlen(buf)+1); */

	/* 						if (g_OptVerbose) { */
	/* 							printf("Wait for Jag to clear %04X\n", nextez); */
	/* 						} */

	/* 						// now we must not proceed from this point until the Jaguar */
	/* 						// acknowledges that block by clearing its length */
	/* 						do { */
	/* 							if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA,  */
	/* 								(char*)&poll, 2, 1000) != 2)) { */
	/* 									assert(0); */
	/* 									printf("Failed to handshake with 68k (control-c to abort)\n"); */
	/* 									Sleep(1000); */
	/* 							} */
	/* 							Sleep(500); */
	/* 						} while (0 != poll); */

	/* 						// Now clear the buffer back to 0xffff so the Jag can use it again */
	/* 						tmp=0xffff; */
	/* 						if (usb_control_msg(udev, 0x40, 0xfe, 4080, nextez+0xFEA,  */
	/* 							(char*)&tmp, 2, 1000) != 2) { */
	/* 								assert(0); */
	/* 								printf("Failed to send block fully (control-c to abort)\n"); */
	/* 								Sleep(500); */
	/* 						} */
	/* 					} */
	/* 					break; */
		
	/* 				case 3:		// Open a file for writing */
	/* 					{ */
	/* 						char buf[4064]; */
	/* 						int i; */

	/* 						for (i=0; i<4060; i++) { */
	/* 							buf[i]=block[i+4]; */
	/* 							if (buf[i]=='\0') break; */
	/* 						} */
	/* 						buf[i]='\0';	// makesure */
	/* 						FilenameSanitize(buf); */

	/* 						if (NULL != fp) { */
	/* 							printf("Closing file...\n"); */
	/* 						} */

	/* 						fp=fopen(buf, "wb"); */
	/* 						if (NULL != fp) { */
	/* 							printf("Opened %s for writing...\n", buf); */
	/* 							nTotalFileLength=0; */
	/* 						} else { */
	/* 							printf("Failed to open %s for writing, code %d\n", buf, errno); */
	/* 						} */
	/* 					} */
	/* 					break; */

	/* 				case 4:		// open a file for reading */
	/* 					{ */
	/* 						char buf[4064]; */
	/* 						int i; */

	/* 						for (i=0; i<4060; i++) { */
	/* 							buf[i]=block[i+4]; */
	/* 							if (buf[i]=='\0') break; */
	/* 						} */
	/* 						buf[i]='\0';	// makesure */
	/* 						FilenameSanitize(buf); */

	/* 						if (NULL != fp) { */
	/* 							printf("Closing file...\n"); */
	/* 						} */

	/* 						fp=fopen(buf, "rb"); */
	/* 						if (NULL != fp) { */
	/* 							printf("Opened %s for reading...\n", buf); */
	/* 						} else { */
	/* 							printf("Failed to open %s for reading, code %d\n", buf, errno); */
	/* 						} */
	/* 					} */
	/* 					break; */

	/* 				case 5:		// write a block to the open file */
	/* 					if (NULL != fp) { */
	/* 						int nLength; */

	/* 						Spin(); */
	/* 						nLength=((block[0xfea]<<8)|block[0xfeb])-4; */
	/* 						if (nLength > 4060) nLength=4060; */
	/* 						fwrite(&block[4], 1, nLength, fp); */
	/* 						nTotalFileLength+=nLength; */
	/* 						if (g_OptVerbose) { */
	/* 							printf("Wrote %d bytes, total %d\n", nLength, nTotalFileLength); */
	/* 						} */
	/* 					} */
	/* 					break; */

	/* 				case 6:		// read a block from a file */
	/* 					if (NULL != fp) { */
	/* 						char buf[4064]; */
	/* 						int nLength; */

	/* 						Spin(); */
	/* 						nLength=(block[0xfea]<<8)|block[0xfeb]; */
	/* 						if (g_OptVerbose) { */
	/* 							printf("Read requested %d -", nLength); */
	/* 						} */
	/* 						if (nLength > 4064) nLength=4064; */
	/* 						nLength=(int)fread(buf, 1, nLength, fp); */
	/* 						if (g_OptVerbose) { */
	/* 							printf(" got %d\n", nLength); */
	/* 						} */
	/* 						// write that input to the jag in the alternate buffer */
	/* 						WriteABlock((unsigned char*)buf, 0, -1, nLength); */

	/* 						if (g_OptVerbose) { */
	/* 							printf("Wait for Jag to clear %04X\n", nextez); */
	/* 						} */
	/* 					} else { */
	/* 						// need to send an empty reply back to the Jag */
	/* 						int nDummy=0; */

	/* 						// write that input to the jag in the alternate buffer */
	/* 						WriteABlock((unsigned char*)&nDummy, 0, -1, 0); */
	/* 					} */

	/* 					// now we must not proceed from this point until the Jaguar */
	/* 					// acknowledges that block by clearing its length */
	/* 					do { */
	/* 						if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA,  */
	/* 							(char*)&poll, 2, 1000) != 2)) { */
	/* 								assert(0); */
	/* 								printf("Failed to handshake with 68k (control-c to abort)\n"); */
	/* 								Sleep(1000); */
	/* 						} */
	/* 					} while (0 != poll); */

	/* 					// Now clear the buffer back to 0xffff so the Jag can use it again */
	/* 					tmp=0xffff; */
	/* 					if (usb_control_msg(udev, 0x40, 0xfe, 4080, nextez+0xFEA,  */
	/* 						(char*)&tmp, 2, 1000) != 2) { */
	/* 							assert(0); */
	/* 							printf("Failed to send block fully (control-c to abort)\n"); */
	/* 							Sleep(500); */
	/* 					} */
	/* 					break; */

	/* 				case 7:		// close a file */
	/* 					if (NULL != fp) { */
	/* 						printf("Closing file...\n"); */
	/* 						fclose(fp); */
	/* 						fp=NULL; */
	/* 					} */
	/* 					break; */
      case 1:
	serve_request(block+4, NULL);
	break;
      case 2: {
	char buf[4064];
	
	serve_request(block+4, buf);
	int nLength = MSGHDRSZ+get_message_length(buf); // add header size to content length

	// write that input to the jag in the alternate buffer
	WriteABlock((unsigned char*)buf, 0, -1, nLength);

	// now we must not proceed from this point until the Jaguar
	// acknowledges that block by clearing its length

	do {
	  if ((usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA,
			       (char*)&poll, 2, 1000) != 2)) {
	    assert(0);
	    printf("Failed to handshake with 68k (control-c to abort)\n");
	    Sleep(1000);
	  }
	} while (0 != poll);
	
	// Now clear the buffer back to 0xffff so the Jag can use it again
	tmp=0xffff;
	if (usb_control_msg(udev, 0x40, 0xfe, 4080, nextez+0xFEA,
			    (char*)&tmp, 2, 1000) != 2) {
	  assert(0);
	  printf("Failed to send block fully (control-c to abort)\n");
	  Sleep(500);
	}

	break;
      }
      default:
	printf("Unimplemented command 0x%04X\n", (block[2]<<8)|block[3]);
	break;
      }
      continue;	
    }

    // else get the length and reformat as a string
    len=block[0xfea]|(block[0xfeb]<<8);
    if (len>4064) len=4064;
    block[len]='\0';
    fprintf(stderr, "%s", (const char*)block);
    //		puts((const char*)block);
  }
}


// remove any path information from buf
void FilenameSanitize(char *buf) {
  char *ptr;

  ptr=strrchr(buf, '/');
  if (NULL != ptr) {
    memmove(buf, ptr, strlen(ptr)+1);
  }
  ptr=strrchr(buf, '\\');
  if (NULL != ptr) {
    memmove(buf, ptr, strlen(ptr)+1);
  }
}

