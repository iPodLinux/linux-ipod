/*
 * ipod_audio.c - audio driver for iPod
 *
 * Copyright (c) 2003,2004 Bernard Leach <leachbj@bouncycastle.org>
 */

#include <linux/module.h>
#include <linux/config.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/major.h>
#include <linux/delay.h>
#include <linux/soundcard.h>
#include <linux/sound.h>
#include <linux/devfs_fs_kernel.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/arch/irqs.h>
#include <asm/arch/hardware.h>

#define D2A_POWER_OFF   1
#define D2A_POWER_SB    2
#define D2A_POWER_ON    3

/* locations for our shared variables */
#define DMA_READ_OFF	0x40000000
#define DMA_WRITE_OFF	0x40000004
#define DMA_ACTIVE	0x40000008
#define DMA_BASE	0x4000000c

/* length of shared buffer in half-words (starting at DMA_BASE) */
#define BUF_LEN		(46*1024)

/* volumes in dB */
#define MAX_VOLUME      6
#define MIN_VOLUME      -73

#define LRHPBOTH        0x100
#define ZERO_DB         0x79

#define LINEIN_ZERO_DB	0x17

static int ipodaudio_isopen;
static int ipodaudio_power_state;
static unsigned ipod_hw_ver;
static devfs_handle_t devfs_handle;
static int ipod_sample_rate;
static int ipodaudio_stereo;

static void
set_clock_enb(unsigned short clks, int on)
{
	if ( on ) {
		outw(inw(0xcf005000) | clks, 0xcf005000);
	}
	else {
		outw(inw(0xcf005000) & ~clks, 0xcf005000);
	}
}

static void
d2a_set_active(int active)
{
	/* set active to 0x0 or 0x1 */
	if ( active == 0 ) {
		ipod_i2c_send(0x1a, 0x12, 0x00);

	} else {
		ipod_i2c_send(0x1a, 0x12, 0x01);
	}
}

static int
ipodaudio_set_sample_rate(int rate)
{
	int sampling_control;

	if (rate <= 8000) {
		if (ipod_hw_ver == 0x3) {
			/* set CLKIDIV2=1 SR=0011 BOSR=0 USB/NORM=1 (USB) */
			sampling_control = 0x4d;
		}
		else {
			/* set CLKIDIV2=1 SR=0001 BOSR=0 USB/NORM=1 (USB) */
			sampling_control = 0x45;
		}
		rate = 8000;
	}
	else if (rate <= 32000) {
		/* set CLKIDIV2=1 SR=0110 BOSR=0 USB/NORM=1 (USB) */
		sampling_control = 0x59;
		rate = 32000;
	}
	else if (rate <= 44100) {
		/* set CLKIDIV2=1 SR=1000 BOSR=1 USB/NORM=1 (USB) */
		sampling_control = 0x63;
		rate = 44100;
	}
	else if (rate <= 48000) {
		/* set CLKIDIV2=1 SR=0000 BOSR=0 USB/NORM=1 (USB) */
		sampling_control = 0x41;
		rate = 48000;
	}
	else if (rate <= 88200) {
		/* set CLKIDIV2=1 SR=1111 BOSR=0 USB/NORM=1 (USB) */
		sampling_control = 0x7f;
		rate = 88200;
	}
	else {
		/* set for 96kHz */
		/* set CLKIDIV2=1 SR=0111 BOSR=0 USB/NORM=1 (USB) */
		sampling_control = 0x5f;
		rate = 96000;
	}

	d2a_set_active(0x0);
	ipod_i2c_send(0x1a, 0x10, sampling_control);
	d2a_set_active(0x1);

	ipod_sample_rate = rate;

	return ipod_sample_rate;
}

static void
d2a_set_power(int new_state)
{
	if ( ipodaudio_power_state == new_state) {
		return;
	}

	if ( new_state != D2A_POWER_OFF ) {
		set_clock_enb((1<<1), 0x1);
	}

	if ( new_state == D2A_POWER_ON ) {
		/* set power register to POWER_OFF=0 on OUTPD=0, DACPD=0 */
		ipod_i2c_send(0x1a, 0xc, 0x67);

		/* de-activate the d2a */
		d2a_set_active(0x0);

		/* set DACSEL=1 */
		if (ipod_hw_ver == 0x3) {
			ipod_i2c_send(0x1a, 0x8, 0x18);
		} else {
			ipod_i2c_send(0x1a, 0x8, 0x10);
		}

		/* set DACMU=0 DEEMPH=0 */
		ipod_i2c_send(0x1a, 0xa, 0x00);

		/* set BCLKINV=0(Dont invert BCLK) MS=1(Enable Master) LRSWAP=0 LRP=0 IWL=10(24 bit) FORMAT=10(I2S format) */
		ipod_i2c_send(0x1a, 0xe, 0x4a);

		ipodaudio_set_sample_rate(ipod_sample_rate);

		/* activate the d2a */
		d2a_set_active(0x1);
	}
	else {
		/* power off or standby the audio chip */

		/* set DACMU=1 DEEMPH=0 */
		ipod_i2c_send(0x1a, 0xa, 0x8);

		/* set DACSEL=0 */
		ipod_i2c_send(0x1a, 0x8, 0x0);

		/* set POWEROFF=0 OUTPD=0 DACPD=1 */
		ipod_i2c_send(0x1a, 0xc, 0x6f);

		if ( new_state == D2A_POWER_OFF ) {
			/* power off the chip */

			/* set POWEROFF=1 OUTPD=1 DACPD=1 */
			ipod_i2c_send(0x1a, 0xc, 0xff);

			set_clock_enb((1<<1), 0x0);
		}
		else {
			/* standby the chip */

			/* set POWEROFF=0 OUTPD=1 DACPD=1 */
			ipod_i2c_send(0x1a, 0xc, 0x7f);
		}
	}

	ipodaudio_power_state = new_state;
}

static void d2a_set_vol(int vol)
{
	unsigned int v;

	if ( vol > MAX_VOLUME ) {
		vol = MAX_VOLUME;
	}

	if ( vol < MIN_VOLUME ) {
		vol = MIN_VOLUME;
	}

	v = (vol + ZERO_DB) | LRHPBOTH;

	/* set volume */
	ipod_i2c_send(0x1a, 0x4 | (v >> 8), v);
}

static void ipodaudio_process_pb_dma(void)
{
	volatile int *r_off = (int *)DMA_READ_OFF;
	volatile int *w_off = (int *)DMA_WRITE_OFF;
	volatile int *dma_active = (int *)DMA_ACTIVE;
	volatile unsigned short *dma_buf = (unsigned short *)DMA_BASE;

	inl(0xcf001040);
	outl(inl(0xc000251c) & ~(1<<9), 0xc000251c);

	while ( *r_off != *w_off ) {
		if ( (inl(0xc000251c) & 0x7800000) == 0 ) {
			outl(inl(0xc000251c)|(1<<9), 0xc000251c);
			return;
		}

		outl(((unsigned)dma_buf[*r_off]) << 16, 0xc0002540);
		if ( !ipodaudio_stereo ) {
			outl(((unsigned)dma_buf[*r_off]) << 16, 0xc0002540);
		}

		*r_off = (*r_off + 1) % BUF_LEN;
	}

	*dma_active = 0;
}

static void ipodaudio_process_rec_dma(void)
{
	volatile int *r_off = (int *)DMA_READ_OFF;
	volatile int *w_off = (int *)DMA_WRITE_OFF;
	volatile int *dma_active = (int *)DMA_ACTIVE;
	volatile unsigned short *dma_buf = (unsigned short *)DMA_BASE;


	inl(0xcf001040);
	outl(inl(0xc000251c) & ~(1<<14), 0xc000251c);

	while ( ((inl(0xc000251c) & 0x78000000)>>27) < 8 ) {
		dma_buf[*w_off] = (unsigned short)(inl(0xc0002580) >> 8);
		if ( !ipodaudio_stereo ) {
			/* throw away second sample */
			inl(0xc0002580);
		}

		*w_off = (*w_off + 1) % BUF_LEN;

		/* check for buffer over run */
		if ( *r_off == *w_off ) {
			*r_off = (*r_off + 1) % BUF_LEN;
		}
	}

	outl(inl(0xc000251c) | (1<<14), 0xc000251c);
	/* *dma_active = 0; */
}

static int ipodaudio_open(struct inode *inode, struct file *filep)
{
	volatile int *r_off = (int *)DMA_READ_OFF;
	volatile int *w_off = (int *)DMA_WRITE_OFF;
	volatile int *dma_active = (int *)DMA_ACTIVE;

	if ( ipodaudio_isopen ) {
		return -EBUSY;
	}

	/* initialise shared variables */
	*r_off = 0;
	*w_off = 0;
	*dma_active = 0;

	ipodaudio_isopen = 1;
	ipod_sample_rate = 44100;
	ipodaudio_stereo = 1;

	/* cop setup */
	if (filep->f_mode & FMODE_WRITE) {
		d2a_set_power(D2A_POWER_ON);

		/* set the volument to -6dB */
		d2a_set_vol(-20);

		if (ipod_hw_ver == 0x3) {
			outl(inl(0xcf000004) & ~0xf, 0xcf000004);
		}

		ipod_set_process_dma(ipodaudio_process_pb_dma);
		outl(inl(0xcf00103c) | (1 << DMA_OUT_IRQ) , 0xcf00103c);
		outl((1 << DMA_OUT_IRQ), 0xcf001034);
	}

	if (filep->f_mode & FMODE_READ) {

		/* 3g recording */
		if (ipod_hw_ver != 0x3) {
			return -ENODEV;
		}

		set_clock_enb((1<<1), 0x1);

		outl(inl(0xcf000004) & ~0xf, 0xcf000004);
		outl(inl(0xcf004044) & ~0x4, 0xcf004044);

		ipod_i2c_send(0x1a, 0x12, 0x0);  /* power off */

		ipod_i2c_send(0x1a, 0x1e, 0x0);  /* reset */

		ipod_i2c_send(0x1a, 0xe, 0x48);  /* MS IWL=24bit FORMAT=MSB */
		ipod_i2c_send(0x1a, 0x10, 0x63); /* CLKI_DIV2 SR=1000 BOSR USB */

#define MICROPHONE
#ifdef MICROPHONE
		/* mic settings */
		ipod_i2c_send(0x1a, 0x0, 0x80);  /* LIN_MUTE */
		ipod_i2c_send(0x1a, 0x2, 0x80);  /* RIN_MUTE */
		ipod_i2c_send(0x1a, 0x4, 0x0);   /* headphone mute (left) */
		ipod_i2c_send(0x1a, 0x6, 0x0);   /* headphone mute (right) */
		ipod_i2c_send(0x1a, 0x8, 0x5);   /* INSEL=mic, MIC_BOOST */
		ipod_i2c_send(0x1a, 0xa, 0x9);   /* DAC_MU, ADC_HPD */

		/* power on (PWR_OFF=0) */
		ipod_i2c_send(0x1a, 0xc, 0x79);  /* CLKOUTPD OSCPD OUTPD DACPD LINEINPD */
#else
		/* line in settings */
#define LINEIN_ZERO_DB	0x17
		ipod_i2c_send(0x1a, 0x0, LINEIN_ZERO_DB);  /* linein volume */
		ipod_i2c_send(0x1a, 0x2, LINEIN_ZERO_DB);  /* linein volume */
		ipod_i2c_send(0x1a, 0x4, 0x0);   /* headphone mute (left) */
		ipod_i2c_send(0x1a, 0x6, 0x0);   /* headphone mute (right) */
		ipod_i2c_send(0x1a, 0x8, 0xa);   /* BY PASS, mute mic, INSEL=line in */

		/* power on (PWR_OFF=0) */
		ipod_i2c_send(0x1a, 0xc, 0x7a);  /* MICPD */
#endif

		ipod_i2c_send(0x1a, 0x12, 0x1);  /* ACTIVE */
	}

	return 0;
}

static void ipodaudio_txdrain(void)
{
	while ( (inl(0xc000251c) & (1<<0)) == 0 ) {
		int to = (32 * HZ * 2) / ipod_sample_rate * 4;
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(to >= 2 ? to : 2);

		if (signal_pending(current)) {
			break;
		}
	}
}

static int ipodaudio_close(struct inode *inode, struct file *filep)
{
	if (filep->f_mode & FMODE_WRITE) {
		ipodaudio_txdrain();

		outl((1 << DMA_OUT_IRQ), 0xcf001038);
		ipod_set_process_dma(0);
	}

	if (filep->f_mode & FMODE_READ) {
		volatile int *dma_active = (int *)DMA_ACTIVE;

		*dma_active = 0;
		outl((1 << DMA_IN_IRQ), 0xcf001038);
		ipod_set_process_dma(0);
	}


	d2a_set_power(D2A_POWER_OFF);

	ipodaudio_isopen = 0;

	return 0;
}


static ssize_t ipodaudio_write(struct file *filp, const char *buf, size_t count, loff_t *ppos)
{
	unsigned short *bufsp;
	size_t rem;

	volatile int *r_off = (int *)DMA_READ_OFF;
	volatile int *w_off = (int *)DMA_WRITE_OFF;
	int write_off_current, write_off_next, read_off_current;
	volatile int *dma_active = (int *)DMA_ACTIVE;
	volatile unsigned short *dma_buf = (unsigned short *)DMA_BASE;

	if ( count <= 0 ) {
		return 0;
	}

	bufsp = (unsigned short *)buf;
	rem = count/2;

	write_off_current = *w_off;

	while ( rem > 0 ) {
		int cnt;

		write_off_next = (write_off_current + 1);
		if ( write_off_next > BUF_LEN ) write_off_next = 0;

		read_off_current = *r_off;

		if ( read_off_current < write_off_current ) {
			cnt = BUF_LEN - 1 - write_off_current;

			if ( cnt > 0 )  {
				if ( cnt > rem ) cnt = rem;

				memcpy((void*)&dma_buf[write_off_next], bufsp, cnt<<1);

				rem -= cnt;
				bufsp += cnt;

				if ( read_off_current > 0 && rem > 0 ) {
					int n;

					if ( rem > read_off_current ) {
						n = read_off_current;
					}
					else {
						n = rem;
					}

					memcpy((void*)&dma_buf[0], bufsp, n<<1);

					rem -= n;
					bufsp += n;

					write_off_current = n - 1;
				}
				else {
					write_off_current += cnt;
				}
			}
			else {
				int to = (100 * HZ * 2) / ipod_sample_rate * 4;

				/* buffer is full */
				set_current_state(TASK_INTERRUPTIBLE);

				/* sleep a little */
				schedule_timeout(to >= 2 ? to : 2);
			}
		}
		else if ( read_off_current > write_off_current ) {
			cnt = read_off_current - 1 - write_off_current;
			if ( cnt > rem ) cnt = rem;

			memcpy((void*)&dma_buf[write_off_next], bufsp, cnt<<1);

			bufsp += cnt;
			rem -= cnt;

			write_off_current += cnt;
		}
		else {
			/* buffer is empty */
			if ( rem < BUF_LEN ) {
				cnt = rem;
			}
			else {
				cnt = BUF_LEN;
			}
			memcpy((void*)&dma_buf[0], bufsp, cnt<<1);

			bufsp += cnt;
			rem -= cnt;

			write_off_current = cnt - 1;

			/* we have copied to the start of the buffer */
			*r_off = 0;
		}

		if ( !*dma_active ) {
			*dma_active = 1;

			*w_off = write_off_current;

			outl(inl(0xc000251c)|(1<<9), 0xc000251c);

			/* dummy write to start things */
			outl(0x0, 0xc0002540);
		}
	}

	*w_off = write_off_current;

	return count;
}

static ssize_t ipodaudio_read(struct file *filp, char *buf, size_t count, loff_t *ppos)
{
	unsigned short *bufsp;
	size_t rem;

	volatile int *r_off = (int *)DMA_READ_OFF;
	volatile int *w_off = (int *)DMA_WRITE_OFF;
	volatile int *dma_active = (int *)DMA_ACTIVE;
	volatile unsigned short *dma_buf = (unsigned short *)DMA_BASE;

	if ( !*dma_active ) {
		*dma_active = 1;

		ipod_set_process_dma(ipodaudio_process_rec_dma);
		outl(inl(0xcf00103c) | (1 << DMA_IN_IRQ) , 0xcf00103c);
		outl((1 << DMA_IN_IRQ), 0xcf001034);

		*r_off = 0;
		*w_off = 0;
		outl(inl(0xc000251c) | 0x20000, 0xc000251c);

		outl(inl(0xc000251c) | (1<<14), 0xc000251c);
	}

	bufsp = (unsigned short *)buf;
	rem = count/2;

	while ( rem > 0 ) {
		int write_pos = *w_off;
		int read_pos = *r_off;
		int len = 0;

		set_current_state(TASK_INTERRUPTIBLE);

		if ( read_pos < write_pos ) {
			len = write_pos - read_pos;
		}
		else if ( write_pos < read_pos ) {
			len = BUF_LEN - read_pos;
		}
		else {
			/* sleep a little */
			int to = (32 * HZ * 2) / ipod_sample_rate * 4;
			schedule_timeout(to >= 2 ? to : 2);
		}

		if ( len > rem ) {
			len = rem;
		}

		if (len) {
			memcpy(buf, (void*)&dma_buf[read_pos], len<<1);

			/* check for buffer over run */
			if ( read_pos == *r_off ) {
				*r_off = (*r_off + len) % BUF_LEN;
			}
			else {
			}

			rem -= len;
		}

		if (signal_pending(current)) {
			set_current_state(TASK_RUNNING);
			return count - (rem * 2);
		}
	}

	set_current_state(TASK_RUNNING);

	return count;
}

static int ipodaudio_ioctl(struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	int val = 0;

	switch (cmd) {
	case SNDCTL_DSP_SPEED:
		rc = verify_area(VERIFY_READ, (void *) arg, sizeof(val));
		if ( rc == 0 ) {
			get_user(val, (int *) arg);

			val = ipodaudio_set_sample_rate(val);

			put_user(val, (int *) arg);
		}
		break;

	case SNDCTL_DSP_GETFMTS:
		rc = verify_area(VERIFY_READ, (void *) arg, sizeof(val));
		if ( rc == 0 ) {
			put_user(AFMT_S16_LE, (int *) arg);
		}
		break;

	case SNDCTL_DSP_SETFMT:
	/* case SNDCTL_DSP_SAMPLESIZE: */
		rc = verify_area(VERIFY_READ, (void *) arg, sizeof(val));
		if ( rc == 0 ) {
			get_user(val, (int *) arg);
			if ( val != AFMT_S16_LE ) {
				put_user(AFMT_S16_LE, (int *) arg);
			}
		}
		break;

	case SNDCTL_DSP_STEREO:
		rc = verify_area(VERIFY_READ, (void *) arg, sizeof(val));
		if ( rc == 0 ) {
			get_user(val, (int *) arg);
			if ( val != 0 && val != 1 ) {
				put_user(1, (int *) arg);
			}
			else {
				ipodaudio_stereo = val;
			}
		}
		break;

	case SNDCTL_DSP_CHANNELS:
		rc = verify_area(VERIFY_READ, (void *) arg, sizeof(val));
		if ( rc == 0 ) {
			get_user(val, (int *) arg);
			if (val > 2) {
				val = 2;
			}
			ipodaudio_stereo = (val == 2);
			put_user(val, (int *) arg);
		}
		break;

	case SNDCTL_DSP_GETBLKSIZE:
		rc = verify_area(VERIFY_WRITE, (void *) arg, sizeof(long));
		if ( rc == 0 ) {
			put_user(BUF_LEN/2, (int *) arg);
		}
		break;

	case SNDCTL_DSP_SYNC:
		rc = 0;
		ipodaudio_txdrain();
		break;

	case SNDCTL_DSP_RESET:
		rc = 0;
		break;
	}

	return rc;
}

static struct file_operations ipodaudio_fops = {
	owner: THIS_MODULE,
	llseek:	no_llseek,
	open: ipodaudio_open,
	release: ipodaudio_close,
	write: ipodaudio_write,
	read: ipodaudio_read,
	ioctl: ipodaudio_ioctl,
};

static int __init ipodaudio_init(void)
{
	printk("ipodaudio: (c) Copyright 2003,2004 Bernard Leach <leachbj@bouncycastle.org>\n");

	devfs_handle = devfs_register(NULL, "dsp", DEVFS_FL_DEFAULT,
			SOUND_MAJOR, SND_DEV_DSP,
			S_IFCHR | S_IWUSR | S_IRUSR,
			&ipodaudio_fops, NULL);
	if (devfs_handle < 0) {
		printk(KERN_WARNING "SOUND: failed to register major %d\n",
			SOUND_MAJOR);
		return 0;
	}

	ipod_hw_ver = ipod_get_hw_version() >> 16;
	if (ipod_hw_ver == 0x3) {
		/* reset I2C */
		ipod_i2c_init();

		/* reset DAC and ADC fifo */
		outl(inl(0xc000251c) | 0x10000, 0xc000251c);
		outl(inl(0xc000251c) | 0x20000, 0xc000251c);
		outl(inl(0xc000251c) & ~0x30000, 0xc000251c);

		/* enable ADC/DAC */
		outl(0xd, 0xc0002500);

		/* bits 11,10 == 01 */
		outl(inl(0xcf004040) | 0x400, 0xcf004040);
		outl(inl(0xcf004040) & ~0x800, 0xcf004040);

		outl(inl(0xcf004048) & ~0x1, 0xcf004048);
	}
	else {
		/* reset DAC fifo */
		outl(inl(0xc000251c) | 0x10000, 0xc000251c);
		outl(inl(0xc000251c) & ~0x10000, 0xc000251c);

		/* enable DAC */
		outl(0x5, 0xc0002500);

		/* nb this is different to 3g!? */
		/* bits 11,10 == 10 */
		outl(inl(0xcf004040) & ~0x400, 0xcf004040);
		outl(inl(0xcf004040) | 0x800, 0xcf004040);
	}

	/* GPIO D bit 6 enable for output */
	outl(inl(0xcf00000c) | 0x40, 0xcf00000c);
	outl(inl(0xcf00001c) & ~0x40, 0xcf00001c);

	return 0;
}

static void __exit ipodaudio_exit(void)
{
	ipod_set_process_dma(0);

	devfs_unregister_chrdev(SOUND_MAJOR, "dsp");
	devfs_unregister(devfs_handle);
}

module_init(ipodaudio_init);
module_exit(ipodaudio_exit);

MODULE_AUTHOR("Bernard Leach <leachbj@bouncycastle.org>");
MODULE_DESCRIPTION("Audio driver for IPod");
MODULE_LICENSE("GPL");
