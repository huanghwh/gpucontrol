#include <err.h>
#include <fcntl.h>
#include <stdio.h>
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


enum discrete_state {STATE_OFF, STATE_ON};
enum gpu_id {IGD, DIS};

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


static void gmux_index_write8(int port, u8 val)
{
	outb(GMUX_IOSTART + GMUX_PORT_VALUE, val);
	gmux_index_wait_ready();
	outb(GMUX_IOSTART + GMUX_PORT_WRITE, (port & 0xff));
	gmux_index_wait_complete();
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

static u32 gmux_index_read32(int port)
{
	u32 val;

	gmux_index_wait_ready();
	outb(GMUX_IOSTART + GMUX_PORT_READ, (port & 0xff));
	gmux_index_wait_complete();

	val = inl(GMUX_IOSTART + GMUX_PORT_VALUE);
	return val;
}

static void set_discrete_state(enum discrete_state state)
{
	if (state == STATE_ON) {	// switch on dGPU
		gmux_index_write8(GMUX_PORT_DISCRETE_POWER, 1);
		gmux_index_write8(GMUX_PORT_DISCRETE_POWER, 3);
	} else {			// switch off dGPU
		gmux_index_write8(GMUX_PORT_DISCRETE_POWER, 1);
		gmux_index_write8(GMUX_PORT_DISCRETE_POWER, 0);
	}
}

static u8 get_discrete_state()
{
	return gmux_index_read8(GMUX_PORT_DISCRETE_POWER);
}

static void switchto(enum gpu_id id)
{
	if (id == IGD) {	// switch to iGPU
		gmux_index_write8(GMUX_PORT_SWITCH_DDC, 1);
		gmux_index_write8(GMUX_PORT_SWITCH_DISPLAY, 2);
		gmux_index_write8(GMUX_PORT_SWITCH_EXTERNAL, 2);
	} else {		// switch to dGPU
		gmux_index_write8(GMUX_PORT_SWITCH_DDC, 2);
		gmux_index_write8(GMUX_PORT_SWITCH_DISPLAY, 3);
		gmux_index_write8(GMUX_PORT_SWITCH_EXTERNAL, 3);
	}
}
static bool gmux_is_indexed()
{
	u16 val;

	outb(GMUX_IOSTART + 0xcc, 0xaa);
	outb(GMUX_IOSTART + 0xcd, 0x55);
	outb(GMUX_IOSTART + 0xce, 0x00);

	val = inb(GMUX_IOSTART + 0xcc) |
		(inb(GMUX_IOSTART + 0xcd) << 8);

	if (val == 0x55aa)
		return true;

	return false;
}

static int gmux_get_brightness()
{
	return gmux_index_read32(GMUX_PORT_BRIGHTNESS) &
	       GMUX_BRIGHTNESS_MASK;
}

static void gmux_set_brightness(u32 brightness)
{
	gmux_index_write32(GMUX_PORT_BRIGHTNESS, brightness);
}

int main(int argc, char **argv)
{
	int fd;
	u8 ver_major, ver_minor, ver_release;
	u32 version, brightness;
	fd = open("/dev/io", O_RDWR);
	if (fd < 0) err(1, "open(/dev/io)");

	version = gmux_index_read32(GMUX_PORT_VERSION_MAJOR);
	ver_major = (version >> 24) & 0xff;
	ver_minor = (version >> 16) & 0xff;
	ver_release = (version >> 8) & 0xff;
	printf("Found gmux version %d.%d.%d\n",
	    ver_major, ver_minor, ver_release);

	//switchto(IGD);
	printf("Discrete state: 0x%X\n", get_discrete_state());
	set_discrete_state(STATE_OFF);
	printf("Discrete state: 0x%X\n", get_discrete_state());
	brightness = gmux_get_brightness();
	printf("brightness=%d\n", brightness);
	gmux_set_brightness(brightness-10);
	close(fd);
	return 0;
}

