#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>
#include <machine/bus.h>

#define GMUX_PORT_VERSION_MAJOR		0x04
#define GMUX_PORT_VERSION_MINOR		0x05
#define GMUX_PORT_VERSION_RELEASE	0x06
#define GMUX_PORT_SWITCH_DISPLAY	0x10
#define GMUX_PORT_SWITCH_GET_DISPLAY	0x11
#define GMUX_PORT_INTERRUPT_ENABLE	0x14
#define GMUX_PORT_INTERRUPT_STATUS	0x16
#define GMUX_PORT_SWITCH_DDC		0x28
#define GMUX_PORT_SWITCH_EXTERNAL	0x40
#define GMUX_PORT_SWITCH_GET_EXTERNAL	0x41
#define GMUX_PORT_DISCRETE_POWER	0x50
#define GMUX_PORT_MAX_BRIGHTNESS	0x70
#define GMUX_PORT_BRIGHTNESS		0x74
#define GMUX_PORT_VALUE			0xc2
#define GMUX_PORT_READ			0xd0
#define GMUX_PORT_WRITE			0xd4

#define GMUX_MIN_IO_LEN			(GMUX_PORT_BRIGHTNESS + 4)

#define GMUX_INTERRUPT_ENABLE		0xff
#define GMUX_INTERRUPT_DISABLE		0x00

#define GMUX_INTERRUPT_STATUS_ACTIVE	0
#define GMUX_INTERRUPT_STATUS_DISPLAY	(1 << 0)
#define GMUX_INTERRUPT_STATUS_POWER	(1 << 2)
#define GMUX_INTERRUPT_STATUS_HOTPLUG	(1 << 3)

#define GMUX_BRIGHTNESS_MASK		0x00ffffff
#define GMUX_MAX_BRIGHTNESS		GMUX_BRIGHTNESS_MASK

#define GMUX_IOSTART		0x700

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

static int indexed = 0;

enum discrete_state {STATE_OFF, STATE_ON};
enum gpu_id {IGD, DIS};

static u8 gmux_pio_read8(int port)
{
	return inb(GMUX_IOSTART + port);
}

static void gmux_pio_write8(int port, u8 val)
{
        outb(GMUX_IOSTART + port, val);
}

static u32 gmux_pio_read32(int port)
{
	return inl(GMUX_IOSTART + port);
}

static void gmux_pio_write32(int port, u32 val)
{
	int i;
	u8 tmpval;

	for (i = 0; i < 4; i++) {
		tmpval = (val >> (i * 8)) & 0xff;
		outb(GMUX_IOSTART + port + i, tmpval);
	}
}

static int gmux_index_wait_ready()
{
	int i = 200;
	u8 gwr = inb(GMUX_IOSTART + GMUX_PORT_WRITE);

	while (i && (gwr & 0x01)) {
		inb(GMUX_IOSTART + GMUX_PORT_READ);
		gwr = inb(GMUX_IOSTART + GMUX_PORT_WRITE);
		usleep(100);
		i--;
	}

	return !!i;
}

static int gmux_index_wait_complete()
{
	int i = 200;
	u8 gwr = inb(GMUX_IOSTART + GMUX_PORT_WRITE);

	while (i && !(gwr & 0x01)) {
		gwr = inb(GMUX_IOSTART + GMUX_PORT_WRITE);
		usleep(100);
		i--;
	}

	if (gwr & 0x01)
		inb(GMUX_IOSTART + GMUX_PORT_READ);

	return !!i;
}

static u8 gmux_index_read8(int port)
{
	u8 val;
	gmux_index_wait_ready();
	outb(GMUX_IOSTART + GMUX_PORT_READ, (port & 0xff));
	gmux_index_wait_complete();
	val = inb(GMUX_IOSTART + GMUX_PORT_VALUE);

	return val;
}

static void gmux_index_write8(int port, u8 val)
{
	outb(GMUX_IOSTART + GMUX_PORT_VALUE, val);
	gmux_index_wait_ready();
	outb(GMUX_IOSTART + GMUX_PORT_WRITE, (port & 0xff));
	gmux_index_wait_complete();
}

static u32 gmux_index_read32(int port)
{
	u32 val;

	gmux_index_wait_ready();
	outb(GMUX_IOSTART + GMUX_PORT_READ, (port & 0xff));
	gmux_index_wait_complete();

	val = inl(GMUX_IOSTART + GMUX_PORT_VALUE);
	return val;
}

static void gmux_index_write32(int port, u32 val)
{
	int i;
	u8 tmpval;

	for (i = 0; i < 4; i++) {
		tmpval = (val >> (i * 8)) & 0xff;
		outb(GMUX_IOSTART + GMUX_PORT_VALUE + i, tmpval);
	}

	gmux_index_wait_ready();
	outb(GMUX_IOSTART + GMUX_PORT_WRITE, (port & 0xff));
	gmux_index_wait_complete();
}

static u8 gmux_read8(int port)
{
	if (indexed)
		return gmux_index_read8(port);
	else
		return gmux_pio_read8(port);
}

static void gmux_write8(int port, u8 val)
{
	if (indexed)
		gmux_index_write8(port, val);
	else
		gmux_pio_write8(port, val);
}

static u32 gmux_read32(int port)
{
	if (indexed)
		return gmux_index_read32(port);
	else
		return gmux_pio_read32(port);
}

static void gmux_write32(int port, u32 val)
{
	if (indexed)
		gmux_index_write32(port, val);
	else
		gmux_pio_write32(port, val);
}

static void set_discrete_state(enum discrete_state state)
{
	if (state == STATE_ON) {	// switch on dGPU
		gmux_write8(GMUX_PORT_DISCRETE_POWER, 1);
		gmux_write8(GMUX_PORT_DISCRETE_POWER, 3);
	} else {			// switch off dGPU
		gmux_write8(GMUX_PORT_DISCRETE_POWER, 1);
		gmux_write8(GMUX_PORT_DISCRETE_POWER, 0);
	}
}

static u8 get_discrete_state()
{
	return gmux_index_read8(GMUX_PORT_DISCRETE_POWER);
}

static void switchto(enum gpu_id id)
{
	if (id == IGD) {	// switch to iGPU
		gmux_write8(GMUX_PORT_SWITCH_DDC, 1);
		gmux_write8(GMUX_PORT_SWITCH_DISPLAY, 2);
		gmux_write8(GMUX_PORT_SWITCH_EXTERNAL, 2);
	} else {		// switch to dGPU
		gmux_write8(GMUX_PORT_SWITCH_DDC, 2);
		gmux_write8(GMUX_PORT_SWITCH_DISPLAY, 3);
		gmux_write8(GMUX_PORT_SWITCH_EXTERNAL, 3);
	}
}
static int gmux_is_indexed()
{
	u16 val;

	outb(GMUX_IOSTART + 0xcc, 0xaa);
	outb(GMUX_IOSTART + 0xcd, 0x55);
	outb(GMUX_IOSTART + 0xce, 0x00);

	val = inb(GMUX_IOSTART + 0xcc) |
		(inb(GMUX_IOSTART + 0xcd) << 8);

	if (val == 0x55aa)
		return 1;

	return 0;
}

static int gmux_get_brightness()
{
	return gmux_read32(GMUX_PORT_BRIGHTNESS) &
	       GMUX_BRIGHTNESS_MASK;
}

static void gmux_set_brightness(u32 brightness)
{
	gmux_write32(GMUX_PORT_BRIGHTNESS, brightness);
}

static void usage(void)
{

	fprintf(stderr, "usage: gpucontrol [-b brightness] [-p]\n");
	exit(1);
}


int main(int argc, char **argv)
{
	int fd;
	int ch;
	int bflag = 0, pflag = 0;
	u8 ver_major, ver_minor, ver_release;
	u32 brightness;
	fd = open("/dev/io", O_RDWR);
	if (fd < 0) err(1, "open(/dev/io)");

	/*
	 * Invalid version information may indicate either that the gmux
	 * device isn't present or that it's a new one that uses indexed
	 * io
	 */

	ver_major = gmux_read8(GMUX_PORT_VERSION_MAJOR);
	ver_minor = gmux_read8(GMUX_PORT_VERSION_MINOR);
	ver_release = gmux_read8(GMUX_PORT_VERSION_RELEASE);
	if (ver_major == 0xff && ver_minor == 0xff && ver_release == 0xff) {
		if (gmux_is_indexed()) {
			u32 version;
			indexed = 1;
			version = gmux_read32(GMUX_PORT_VERSION_MAJOR);
			ver_major = (version >> 24) & 0xff;
			ver_minor = (version >> 16) & 0xff;
			ver_release = (version >> 8) & 0xff;
		} else {
			printf("gmux device not present or IO disabled\n");
			return -1;
		}
	}
	printf("Found gmux version %d.%d.%d [%s]\n", ver_major, ver_minor,
		ver_release, (indexed ? "indexed" : "classic"));

	brightness = gmux_get_brightness();
	printf("brightness: %d\n", brightness);
	printf("Discrete state: 0x%X\n\n", get_discrete_state());
	
	while ((ch = getopt(argc, argv, "b:p")) != -1)
		switch (ch) {
		case 'b':
			bflag = 1;
			break;
		case 'p':
			pflag = 1;
			break;
		default:
			usage();
		}
	argv += optind;

	if (bflag) {
		brightness =  atoi(optarg);
		gmux_set_brightness(brightness);	  
		printf("Set brightness: %d\n", brightness);
	}
	if (pflag){
		switchto(IGD);
		set_discrete_state(STATE_OFF);
		printf("Now Discrete state: 0x%X\n", get_discrete_state());
	}

	close(fd);
	return 0;
}


