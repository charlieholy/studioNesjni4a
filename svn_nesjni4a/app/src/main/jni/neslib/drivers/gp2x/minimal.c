/**
 * This is a rewrite of a subset of rlyeh's minimal library.
**/

/*

  GP2X minimal library v0.A by rlyeh, (c) 2005. emulnation.info@rlyeh (swap it!)

  Thanks to Squidge, Robster, snaff, Reesy and NK, for the help & previous work! :-)

  License
  =======

  Free for non-commercial projects (it would be nice receiving a mail from you).
  Other cases, ask me first.

  GamePark Holdings is not allowed to use this library and/or use parts from it.

*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <fcntl.h>
#include <errno.h>

#include "minimal.h"
#include "usbjoy.h"
#include "asmutils.h"

volatile unsigned short *gp2x_memregs;
//static
volatile unsigned long  *gp2x_memregl;
static void *gp2x_screens[4];
static int screensel = 0;
static int gp2x_screen_offs = 0;
//static
int memdev = 0;
static int sounddev = 0, mixerdev = 0;

void *gp2x_screen;

#define FRAMEBUFF_WHOLESIZE (0x30000*4) // 320*240*2 + some more
#define FRAMEBUFF_ADDR0 (0x4000000-FRAMEBUFF_WHOLESIZE)
#define FRAMEBUFF_ADDR1 (FRAMEBUFF_ADDR0+0x30000)
#define FRAMEBUFF_ADDR2 (FRAMEBUFF_ADDR1+0x30000)
#define FRAMEBUFF_ADDR3 (FRAMEBUFF_ADDR2+0x30000)

static const int gp2x_screenaddrs[4] = { FRAMEBUFF_ADDR0, FRAMEBUFF_ADDR1, FRAMEBUFF_ADDR2, FRAMEBUFF_ADDR3 };
static unsigned short gp2x_screenaddr_old[4];


/* video stuff */
void gp2x_video_flip(void)
{
	unsigned short lsw, msw;
	int addr = gp2x_screenaddrs[screensel&3];

	addr += gp2x_screen_offs;

	lsw = (unsigned short) addr;
	msw = (unsigned short)(addr >> 16);

  	gp2x_memregs[0x2910>>1] = msw;
  	gp2x_memregs[0x2914>>1] = msw;
	gp2x_memregs[0x290E>>1] = lsw;
  	gp2x_memregs[0x2912>>1] = lsw;

	// jump to other buffer:
	gp2x_screen = gp2x_screens[++screensel&3];
}

/* doulblebuffered flip */
void gp2x_video_flip2(void)
{
	unsigned short msw = (unsigned short)(gp2x_screenaddrs[screensel&1] >> 16);

  	gp2x_memregs[0x2910>>1] = msw;
  	gp2x_memregs[0x2914>>1] = msw;
	gp2x_memregs[0x290E>>1] = 0;
  	gp2x_memregs[0x2912>>1] = 0;

	// jump to other buffer:
	gp2x_screen = gp2x_screens[++screensel&1];
}


void gp2x_video_changemode2(int bpp)
{
  	gp2x_memregs[0x28DA>>1]=(((bpp+1)/8)<<9)|0xAB; /*8/15/16/24bpp...*/
  	gp2x_memregs[0x290C>>1]=320*((bpp+1)/8); /*line width in bytes*/
}


void gp2x_video_changemode(int bpp)
{
	gp2x_video_changemode2(bpp);

  	gp2x_memset_all_buffers(0, 0, 320*240*2);
	gp2x_video_flip();
}


void gp2x_video_setpalette(int *pal, int len)
{
	unsigned short *g=(unsigned short *)pal;
	volatile unsigned short *memreg = &gp2x_memregs[0x295A>>1];
	gp2x_memregs[0x2958>>1] = 0;

	len *= 2;
	while(len--) *memreg=*g++;
}


// TV Compatible function //
void gp2x_video_RGB_setscaling(int W, int H)
{
	float escalaw, escalah;
	int bpp = (gp2x_memregs[0x28DA>>1]>>9)&0x3;
	unsigned short scalw;

	escalaw = 1024.0; // RGB Horiz LCD
	escalah = 320.0; // RGB Vert LCD

	if(gp2x_memregs[0x2800>>1]&0x100) //TV-Out
	{
		escalaw=489.0; // RGB Horiz TV (PAL, NTSC)
		if (gp2x_memregs[0x2818>>1]  == 287) //PAL
			escalah=274.0; // RGB Vert TV PAL
		else if (gp2x_memregs[0x2818>>1]  == 239) //NTSC
			escalah=331.0; // RGB Vert TV NTSC
	}

	// scale horizontal
	scalw = (unsigned short)((float)escalaw *(W/320.0));
	/* if there is no horizontal scaling, vertical doesn't work. Here is a nasty wrokaround... */
	if (H != 240 && W == 320) scalw--;
	gp2x_memregs[0x2906>>1]=scalw;
	// scale vertical
	gp2x_memregl[0x2908>>2]=(unsigned long)((float)escalah *bpp *(H/240.0));
}

void gp2x_video_set_offs(int offs)
{
	gp2x_screen_offs = offs;
}

void gp2x_video_wait_vsync(void)
{
	unsigned short v = gp2x_memregs[0x1182>>1];
	while (!((v ^ gp2x_memregs[0x1182>>1]) & 0x10));
}

void gp2x_video_flush_cache(void)
{
	// since we are using the mmu hack, we must flush the cache first
	// (the params are most likely wrong, but they seem to work somehow)
	//flushcache(addr, addr + 320*240*2, 0);
	flushcache(gp2x_screen, (char *)gp2x_screen + 320*240*2, 0);
}

void gp2x_memcpy_buffers(int buffers, void *data, int offset, int len)
{
	if (buffers & (1<<0)) memcpy((char *)gp2x_screens[0] + offset, data, len);
	if (buffers & (1<<1)) memcpy((char *)gp2x_screens[1] + offset, data, len);
	if (buffers & (1<<2)) memcpy((char *)gp2x_screens[2] + offset, data, len);
	if (buffers & (1<<3)) memcpy((char *)gp2x_screens[3] + offset, data, len);
}


void gp2x_memcpy_all_buffers(void *data, int offset, int len)
{
	gp2x_memcpy_buffers(0xf, data, offset, len);
}


void gp2x_memset_all_buffers(int offset, int byte, int len)
{
	memset((char *)gp2x_screens[0] + offset, byte, len);
	memset((char *)gp2x_screens[1] + offset, byte, len);
	memset((char *)gp2x_screens[2] + offset, byte, len);
	memset((char *)gp2x_screens[3] + offset, byte, len);
}


unsigned long gp2x_joystick_read(int allow_usb_joy)
{
	int i;
  	unsigned long value=(gp2x_memregs[0x1198>>1] & 0x00FF);
  	if(value==0xFD) value=0xFA;
  	if(value==0xF7) value=0xEB;
  	if(value==0xDF) value=0xAF;
  	if(value==0x7F) value=0xBE;
  	value = ~((gp2x_memregs[0x1184>>1] & 0xFF00) | value | (gp2x_memregs[0x1186>>1] << 16));

	if (allow_usb_joy && num_of_joys > 0) {
		// check the usb joy as well..
		gp2x_usbjoy_update();
		for (i = 0; i < num_of_joys; i++)
			value |= gp2x_usbjoy_check(i);
	}

	return value;
}

static int s_oldrate = 0, s_oldbits = 0, s_oldstereo = 0;

void gp2x_start_sound(int rate, int bits, int stereo)
{
	int frag = 0, bsize, buffers;

	// if no settings change, we don't need to do anything
	if (rate == s_oldrate && s_oldbits == bits && s_oldstereo == stereo) return;

	if (sounddev > 0) close(sounddev);
	sounddev = open("/dev/dsp", O_WRONLY|O_ASYNC);
	if (sounddev == -1)
	{
		printf("open(\"/dev/dsp\") failed with %i\n", errno);
		return;
	}

	ioctl(sounddev, SNDCTL_DSP_SPEED,  &rate);
	ioctl(sounddev, SNDCTL_DSP_SETFMT, &bits);
	ioctl(sounddev, SNDCTL_DSP_STEREO, &stereo);
	// calculate buffer size
	buffers = 16;
	bsize = rate / 32;
	if (rate > 22050) { bsize*=4; buffers*=2; } // 44k mode seems to be very demanding
	while ((bsize>>=1)) frag++;
	frag |= buffers<<16; // 16 buffers
	ioctl(sounddev, SNDCTL_DSP_SETFRAGMENT, &frag);
	printf("gp2x_set_sound: %i/%ibit/%s, %i buffers of %i bytes\n",
		rate, bits, stereo?"stereo":"mono", frag>>16, 1<<(frag&0xffff));

	s_oldrate = rate; s_oldbits = bits; s_oldstereo = stereo;
	usleep(100000);
}


void gp2x_sound_write(void *buff, int len)
{
	write(sounddev, buff, len);
}

void gp2x_sound_sync(void)
{
	ioctl(sounddev, SOUND_PCM_SYNC, 0);
}

void gp2x_sound_volume(int l, int r)
{
 	l=l<0?0:l; l=l>255?255:l; r=r<0?0:r; r=r>255?255:r;
 	l<<=8; l|=r;
 	ioctl(mixerdev, SOUND_MIXER_WRITE_PCM, &l); /*SOUND_MIXER_WRITE_VOLUME*/
}


/* common */
void gp2x_init(void)
{
	printf("entering init()\n"); fflush(stdout);

  	memdev = open("/dev/mem", O_RDWR);
	if (memdev == -1)
	{
		printf("open(\"/dev/mem\") failed with %i\n", errno);
		exit(1);
	}

	gp2x_memregs = mmap(0, 0x10000, PROT_READ|PROT_WRITE, MAP_SHARED, memdev, 0xc0000000);
	printf("memregs are @ %p\n", gp2x_memregs);
	if(gp2x_memregs == MAP_FAILED)
	{
		printf("mmap(memregs) failed with %i\n", errno);
		exit(1);
	}
	gp2x_memregl = (unsigned long *) gp2x_memregs;

  	gp2x_screens[0] = mmap(0, FRAMEBUFF_WHOLESIZE, PROT_WRITE, MAP_SHARED, memdev, FRAMEBUFF_ADDR0);
	if(gp2x_screens[0] == MAP_FAILED)
	{
		printf("mmap(gp2x_screen) failed with %i\n", errno);
		exit(1);
	}
	printf("framebuffers point to %p\n", gp2x_screens[0]);
	gp2x_screens[1] = (char *) gp2x_screens[0]+0x30000;
	gp2x_screens[2] = (char *) gp2x_screens[1]+0x30000;
	gp2x_screens[3] = (char *) gp2x_screens[2]+0x30000;

	gp2x_screen = gp2x_screens[0];
	screensel = 0;

	gp2x_screenaddr_old[0] = gp2x_memregs[0x290E>>1];
	gp2x_screenaddr_old[1] = gp2x_memregs[0x2910>>1];
	gp2x_screenaddr_old[2] = gp2x_memregs[0x2912>>1];
	gp2x_screenaddr_old[3] = gp2x_memregs[0x2914>>1];

	gp2x_memset_all_buffers(0, 0, 320*240*2);

	// snd
  	mixerdev = open("/dev/mixer", O_RDWR);
	if (mixerdev == -1)
		printf("open(\"/dev/mixer\") failed with %i\n", errno);

	/* init usb joys -GnoStiC */
	gp2x_usbjoy_init();

	printf("exitting init()\n"); fflush(stdout);
}

char *ext_menu = 0, *ext_state = 0;

void gp2x_deinit(void)
{
	gp2x_video_changemode(15);
	gp2x_memregs[0x290E>>1] = gp2x_screenaddr_old[0];
	gp2x_memregs[0x2910>>1] = gp2x_screenaddr_old[1];
	gp2x_memregs[0x2912>>1] = gp2x_screenaddr_old[2];
	gp2x_memregs[0x2914>>1] = gp2x_screenaddr_old[3];

	gp2x_usbjoy_deinit();

	munmap(gp2x_screens[0], FRAMEBUFF_WHOLESIZE);
	munmap((void *)gp2x_memregs, 0x10000);
	close(memdev);
	close(mixerdev);
	if (sounddev > 0) close(sounddev);

	printf("all done, running ");

	// Zaq121's alternative frontend support from MAME
	if(ext_menu && ext_state) {
		printf("%s -state %s\n", ext_menu, ext_state);
		execl(ext_menu, ext_menu, "-state", ext_state, NULL);
	} else if(ext_menu) {
		printf("%s\n", ext_menu);
		execl(ext_menu, ext_menu, NULL);
	} else {
		printf("gp2xmenu\n");
		chdir("/usr/gp2x");
		execl("gp2xmenu", "gp2xmenu", NULL);
	}
}


