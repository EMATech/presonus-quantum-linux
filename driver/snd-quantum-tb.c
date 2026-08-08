/*
 * ALSA PCI driver skeleton for PreSonus Quantum family Thunderbolt audio interfaces
 * PCI ID 1c67:0101 (Quantum), 0102 (Quantum 2), 0103 (Quantum 4848), 0104 (Quantum 2626) & 0105 (Unreleased Mobile) from Windows INF.
 *
 * Based on: kernel Documentation/sound/kernel-api/writing-an-alsa-driver.rst
 * and sound/pci/ens1370.c structure. No hardware access yet — BAR mapped,
 * card + stub PCM only. Real PCM/IRQ behavior needs reverse engineering.
 *
 * SPDX-FileCopyrightText: 2026 Jamie Steele
 * SPDX-FileCopyrightText: 2026 Raphaël Doursenaud
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define DRV_NAME                "snd-quantum"
#define QUANTUM_NAMELONG        "PreSonus Quantum"
#define QUANTUM2_NAMELONG       "PreSonus Quantum 2"
#define QUANTUM4848_NAMELONG    "PreSonus Quantum 4848"
#define QUANTUM2626_NAMELONG    "PreSonus Quantum 2626"
#define QUANTUM_MOBILE_NAMELONG "Presonus Quantum Mobile" /* Unreleased prototype? */

/* PreSonus PCI vendor ID (from driver-reference/pae_quantum.inf) */
#define PCI_VENDOR_ID_PRESONUS          0x1c67
#define PCI_DEVICE_ID_QUANTUM       	0x0101
#define PCI_DEVICE_ID_QUANTUM2	        0x0102
#define PCI_DEVICE_ID_QUANTUM4848   	0x0103
#define PCI_DEVICE_ID_QUANTUM2626   	0x0104
#define PCI_DEVICE_ID_QUANTUM_MOBILE	0x0105

/* TODO:
 * - [ ] Init Quantum (Blue LED)
 * - [ ] Read FW version/revision
 * - [ ] Hardware controls
 *   - [ ] Power LED
 *   - [ ] Set sample rate
 *   - [ ] Set sync source (Internal/WordClock/SPDIF/ADAT)
 *   - [ ] Gain (Per channel)
 *   - [ ] 48V enable/disable (Per channel)
 *   - [ ] Talkback (Quantum only)
 *   - [ ] A/B (Quantum 2 only)
 * - [ ] Per model (and per sample rate?) channel counts
 * - [ ] Per model (and per sample rate?) channel maps
 * - [ ] MIDI
 * - [ ] Meters
 * - [ ] Multiple cards aggregation (Up to 4?)
 */

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for PreSonus Quantum card.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for PreSonus Quantum card.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable PreSonus Quantum card.");

static bool dump_on_trigger;
module_param(dump_on_trigger, bool, 0444);
MODULE_PARM_DESC(dump_on_trigger, "Dump MMIO at prepare/trigger for reverse-engineering (default off).");

/* Register access for reverse engineering */
static int reg_read_offset = -1;
module_param(reg_read_offset, int, 0644);
MODULE_PARM_DESC(reg_read_offset, "MMIO offset to read (hex, -1 to disable). Result in dmesg.");

static int reg_write_offset = -1;
module_param(reg_write_offset, int, 0644);
MODULE_PARM_DESC(reg_write_offset, "MMIO offset to write (hex, -1 to disable).");

static int reg_write_value = 0;
module_param(reg_write_value, int, 0644);
MODULE_PARM_DESC(reg_write_value, "Value to write to reg_write_offset (hex).");

static bool reg_scan;
module_param(reg_scan, bool, 0644);
MODULE_PARM_DESC(reg_scan, "Scan and dump first 256 bytes of MMIO (0x00-0xff).");

/* Optional: program these when offset discovered (Ghidra / Windows capture). -1 = disabled. */
static int reg_srate_offset = -1;
module_param(reg_srate_offset, int, 0644);
MODULE_PARM_DESC(reg_srate_offset, "MMIO offset for sample rate (hex, -1 disabled). Set with reg_srate_value.");

static int reg_srate_value = 48000;
module_param(reg_srate_value, int, 0644);
MODULE_PARM_DESC(reg_srate_value, "Value for sample rate reg (e.g. 48000). -1 = use runtime rate when offset set.");

static int reg_bufsize_offset = -1;
module_param(reg_bufsize_offset, int, 0644);
MODULE_PARM_DESC(reg_bufsize_offset, "MMIO offset for buffer size in bytes (hex, -1 disabled).");

static int reg_fmt_offset = -1;
module_param(reg_fmt_offset, int, 0644);
MODULE_PARM_DESC(reg_fmt_offset, "MMIO offset for format (hex, -1 disabled).");

static int reg_fmt_value = 0;
module_param(reg_fmt_value, int, 0644);
MODULE_PARM_DESC(reg_fmt_value, "Value for format reg. -1 = use runtime format width (bits) when offset set.");

/* Override value written to CONTROL 0x100 on stream start. -1 = use default 0x8 (from Ghidra). */
static int control_value = -1;
module_param(control_value, int, 0644);
MODULE_PARM_DESC(control_value, "Value for CONTROL 0x100 on start (-1 = 0x8). Try 0x88, 0x9, etc. if no sound.");

/* Stream-start path also writes 0x8 and 0x10 (values unknown). Set >= 0 to try. */
static int reg_status2_value = -1;
module_param(reg_status2_value, int, 0644);
MODULE_PARM_DESC(reg_status2_value, "If >= 0, write to STATUS2 0x8 on prepare (hex). -1 = don't write.");

static int reg_status3_value = -1;
module_param(reg_status3_value, int, 0644);
MODULE_PARM_DESC(reg_status3_value, "If >= 0, write to STATUS3 0x10 on prepare (hex). -1 = don't write.");

/* Run device init sequence at probe (reads + init writes). Needed for blue LED / audio. */
static bool do_init_sequence = true;
module_param(do_init_sequence, bool, 0644);
MODULE_PARM_DESC(do_init_sequence, "Run init sequence at probe. Set 0 to disable.");

/* Value for final CONTROL (0x100) write at init. -1 = use 0x8. Try 0x88, 0x10, 0x9 for LED. */
static int init_control_value = -1;
module_param(init_control_value, int, 0644);
MODULE_PARM_DESC(init_control_value, "Init CONTROL 0x100 value (hex). -1 = 0x8. Try 0x88, 0x10 if LED blinks.");

MODULE_AUTHOR("Quantum Thunderbolt Family Linux Driver Project");
MODULE_DESCRIPTION("PreSonus Quantum Thunderbolt Family ALSA (PCIe) driver (skeleton)");
MODULE_LICENSE("GPL");

/*
 * Register map from notes/GHIDRA_FINDINGS_SUMMARY.md and REGISTER_GUESSES.md.
 * stream_start_writes.txt may be empty if Ghidra control function name differs; use these offsets.
 * Still missing (set via module params when found): sample rate reg, format reg, buffer size reg.
 */
#define QUANTUM_REG_VERSION     0x0000  /* Version/ID register */
#define QUANTUM_REG_STATUS1     0x0004  /* Status/Control */
#define QUANTUM_REG_STATUS2     0x0008  /* Status/Control */
#define QUANTUM_REG_STATUS3     0x0010  /* Status/Control */
#define QUANTUM_REG_STATUS4     0x0014  /* Status/Control */
#define QUANTUM_REG_CONTROL     0x0100  /* Control register (write 0x8) */
#define QUANTUM_REG_STATUS5     0x0104  /* Status/Control */
#define QUANTUM_REG_DMA_RESET   0x11004 /* Critical: DMA/FIFO Reset observed in Windows init */
#define QUANTUM_REG_BUFFER0     0x10300 /* Buffer/Channel register (DMA buffer address) */
#define QUANTUM_REG_BUFFER1     0x10304 /* Buffer/Channel register (DMA buffer address) */

struct quantum_chip {
	bool disconnected;
	struct snd_card *card;
	struct pci_dev *pci;
	void __iomem *iobase;	/* BAR 0, 1 MiB from lspci */
	int irq;
	bool msi_allocated;	/* Thunderbolt only supports MSI. No Legacy IRQ! */
	struct snd_pcm *pcm;
	struct snd_pcm_substream *playback_substream;
	struct snd_pcm_substream *capture_substream;

	/* Hardware state */
	dma_addr_t playback_dma_addr;
	dma_addr_t capture_dma_addr;
	size_t playback_buffer_size;
	size_t capture_buffer_size;

	/* Model identification */
	const char *model_name;
};

/* Per-substream: fake hw pointer so stream runs (timer-driven until we have real IRQ) */
struct quantum_runtime {
	spinlock_t lock;
	snd_pcm_uframes_t position;
	struct timer_list timer;
	struct snd_pcm_substream *substream;
	bool running;
};

/* ----- PCM ops: timer-driven fake pointer so aplay/arecord can run ----- */

static const struct snd_pcm_hardware quantum_pcm_hw = {
	.info = (SNDRV_PCM_INFO_BLOCK_TRANSFER |
             SNDRV_PCM_INFO_INTERLEAVED |
	         SNDRV_PCM_INFO_MMAP |
			 SNDRV_PCM_INFO_MMAP_VALID),
	 /* FIXME: should be 24 or 32 bits */
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = (SNDRV_PCM_RATE_44100 |
	          SNDRV_PCM_RATE_48000 |
		      SNDRV_PCM_RATE_88200 |
			  SNDRV_PCM_RATE_96000 |
		      SNDRV_PCM_RATE_176400 |
			  SNDRV_PCM_RATE_192000),
	.rate_min = 44100,
	.rate_max = 192000,
	.channels_min = 2, /* FIXME: to determine */
	.channels_max = 2, /* FIXME: 48? */
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = 256,
	.period_bytes_max = 256 * 1024,
	.periods_min = 2,
	.periods_max = 1024,
};

/* Initialize model-specific data in the chip structure */
static void quantum_init_model_data(struct quantum_chip *chip, struct pci_dev *pci)
{
	switch (pci->device) {
		case PCI_DEVICE_ID_QUANTUM:
			chip->model_name = QUANTUM_NAMELONG;
			break;
		case PCI_DEVICE_ID_QUANTUM2:
			chip->model_name = QUANTUM2_NAMELONG;
			break;
		case PCI_DEVICE_ID_QUANTUM4848:
			chip->model_name = QUANTUM4848_NAMELONG;
			break;
		case PCI_DEVICE_ID_QUANTUM2626:
			chip->model_name = QUANTUM2626_NAMELONG;
			break;
		case PCI_DEVICE_ID_QUANTUM_MOBILE:
			chip->model_name = QUANTUM_MOBILE_NAMELONG;
			dev_warn(&pci->dev, "Unsupported Quantum device ID 0x%04x! Unreleased hardware.\n", pci->device);
			break;
		default:
			chip->model_name = QUANTUM_NAMELONG;
			dev_warn(&pci->dev, "Unknown Quantum device ID 0x%04x, using generic name\n", pci->device);
			break;
	}
}

static void quantum_period_elapsed(struct timer_list *t)
{
	struct quantum_runtime *qr = container_of(t, struct quantum_runtime, timer);
	struct snd_pcm_substream *substream = qr->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t period_frames = runtime->period_size;
	snd_pcm_uframes_t buf_frames = runtime->buffer_size;
	unsigned long flags;

	if (!qr->running)
		return;
	spin_lock_irqsave(&qr->lock, flags);
	qr->position += period_frames;
	if (qr->position >= buf_frames)
		qr->position -= buf_frames;
	spin_unlock_irqrestore(&qr->lock, flags);
	snd_pcm_period_elapsed(substream);
	mod_timer(&qr->timer, jiffies + msecs_to_jiffies((period_frames * 1000) / runtime->rate));
}

static int quantum_pcm_open(struct snd_pcm_substream *substream)
{
	struct quantum_runtime *qr;

	qr = kzalloc(sizeof(*qr), GFP_KERNEL);
	if (!qr)
		return -ENOMEM;
	spin_lock_init(&qr->lock);
	qr->substream = substream;
	timer_setup(&qr->timer, quantum_period_elapsed, 0);
	substream->runtime->private_data = qr;
	substream->runtime->hw = quantum_pcm_hw;
	return 0;
}

static int quantum_pcm_close(struct snd_pcm_substream *substream)
{
	struct quantum_runtime *qr = substream->runtime->private_data;

	if (qr) {
		qr->running = false;
		timer_delete_sync(&qr->timer);
		kfree(qr);
		substream->runtime->private_data = NULL;
	}
	return 0;
}

static int quantum_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params)
{
	struct quantum_chip *chip = substream->pcm->private_data;

	/* Store buffer size for later use in prepare() */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		chip->playback_buffer_size = params_buffer_bytes(hw_params);
	else
		chip->capture_buffer_size = params_buffer_bytes(hw_params);

	return 0;
}

static int quantum_pcm_hw_free(struct snd_pcm_substream *substream)
{
	(void)substream;
	return 0;
}

static int quantum_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct quantum_chip *chip = substream->pcm->private_data;
	struct quantum_runtime *qr = substream->runtime->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	dma_addr_t dma_addr;
	size_t buffer_size;
	int i;
	u32 val;

	if (qr)
		qr->position = 0;

	if (!chip->iobase)
		return -EIO;

	/* Get DMA address and buffer size */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		dma_addr = runtime->dma_addr;
		buffer_size = chip->playback_buffer_size;
		chip->playback_dma_addr = dma_addr;
	} else {
		dma_addr = runtime->dma_addr;
		buffer_size = chip->capture_buffer_size;
		chip->capture_dma_addr = dma_addr;
	}

	/* Program DMA buffer address to hardware */
	/* Based on Ghidra analysis: 0x10300 and 0x10304 are buffer registers */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		/* Write buffer address (lower 32 bits) */
		wmb();
		writel((u32)(dma_addr & 0xffffffff), chip->iobase + QUANTUM_REG_BUFFER0);
		dev_info(&chip->pci->dev, "Prepare playback: dma_addr=0x%llx buffer_size=%zu -> 0x%x\n",
			 (unsigned long long)dma_addr, buffer_size, (u32)(dma_addr & 0xffffffff));
		/* Note: May need upper 32 bits or buffer size register */
	} else {
		/* Capture buffer */
		wmb();
		writel((u32)(dma_addr & 0xffffffff), chip->iobase + QUANTUM_REG_BUFFER1);
		dev_info(&chip->pci->dev, "Prepare capture: dma_addr=0x%llx buffer_size=%zu -> 0x%x\n",
			 (unsigned long long)dma_addr, buffer_size, (u32)(dma_addr & 0xffffffff));
	}

	/* Read initial status registers (from Ghidra: reads 0x0, 0x4, 0x8, 0x10, 0x14, 0x104) */
	val = readl(chip->iobase + QUANTUM_REG_VERSION);
	dev_dbg(&chip->pci->dev, "Version reg (0x%04x): 0x%08x\n", QUANTUM_REG_VERSION, val);

	val = readl(chip->iobase + QUANTUM_REG_STATUS1);
	dev_dbg(&chip->pci->dev, "Status1 (0x%04x): 0x%08x\n", QUANTUM_REG_STATUS1, val);

	val = readl(chip->iobase + QUANTUM_REG_STATUS5);
	dev_dbg(&chip->pci->dev, "Status5 (0x%04x): 0x%08x\n", QUANTUM_REG_STATUS5, val);

	/* Optional: stream-start path writes to 0x8 and 0x10 (trace value unknown) */
	if (reg_status2_value >= 0) {
		wmb();
		writel((u32)reg_status2_value, chip->iobase + QUANTUM_REG_STATUS2);
		dev_info(&chip->pci->dev, "Prepare: STATUS2 0x8 = 0x%x\n", (u32)reg_status2_value);
	}
	if (reg_status3_value >= 0) {
		wmb();
		writel((u32)reg_status3_value, chip->iobase + QUANTUM_REG_STATUS3);
		dev_info(&chip->pci->dev, "Prepare: STATUS3 0x10 = 0x%x\n", (u32)reg_status3_value);
	}

	/* Write control register (from Ghidra: 0x100 = 0x8; overridable via control_value) */
	{
		u32 ctrl = (control_value >= 0) ? (u32)control_value : 0x8u;
		wmb();
		writel(ctrl, chip->iobase + QUANTUM_REG_CONTROL);
		dev_info(&chip->pci->dev, "Prepare: CONTROL 0x100 = 0x%x (rate=%u format=%u)\n",
			 ctrl, runtime->rate, (unsigned int)snd_pcm_format_width(runtime->format));
	}

	/* Optional: program when offset found via Ghidra / Windows capture */
	if (reg_srate_offset >= 0) {
		u32 srate = (reg_srate_value >= 0) ? (u32)reg_srate_value : (u32)runtime->rate;
		wmb();
		writel(srate, chip->iobase + reg_srate_offset);
		dev_info(&chip->pci->dev, "Prepare: sample rate reg 0x%x = %u\n",
			 reg_srate_offset, srate);
	}
	if (reg_bufsize_offset >= 0) {
		wmb();
		writel((u32)buffer_size, chip->iobase + reg_bufsize_offset);
		dev_info(&chip->pci->dev, "Prepare: buffer size reg 0x%x = %zu\n",
			 reg_bufsize_offset, buffer_size);
	}
	if (reg_fmt_offset >= 0) {
		wmb();
		u32 fmt = (reg_fmt_value >= 0) ? (u32)reg_fmt_value : (u32)snd_pcm_format_width(runtime->format);
		writel(fmt, chip->iobase + reg_fmt_offset);
		dev_info(&chip->pci->dev, "Prepare: format reg 0x%x = %u\n",
			 reg_fmt_offset, fmt);
	}

	if (dump_on_trigger) {
		dev_info(&chip->pci->dev, "MMIO at prepare:");
		for (i = 0; i < 64; i += 4)
			dev_info(&chip->pci->dev, "MMIO+0x%02x: 0x%08x\n", i, readl(chip->iobase + i));
	}

	return 0;
}

static int quantum_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct quantum_chip *chip = substream->pcm->private_data;
	struct quantum_runtime *qr = substream->runtime->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long period_msec;
	u32 control_val;
	int i;

	if (!qr)
		return -EINVAL;

	if (!chip->iobase)
		return -EIO;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (dump_on_trigger) {
			dev_info(&chip->pci->dev, "MMIO at trigger START:");
			for (i = 0; i < 64; i += 4)
				dev_info(&chip->pci->dev, "MMIO+0x%02x: 0x%08x\n", i, readl(chip->iobase + i));
		}

		/* Start hardware stream */
		{
			u32 ctrl = (control_value >= 0) ? (u32)control_value : 0x8u;
			control_val = readl(chip->iobase + QUANTUM_REG_CONTROL);
		    wmb();
			writel(ctrl, chip->iobase + QUANTUM_REG_CONTROL);
			dev_info(&chip->pci->dev, "Trigger START %s: CONTROL 0x100 was 0x%x now 0x%x\n",
				 substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
				 control_val, ctrl);
		}

		qr->running = true;
		qr->position = 0;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			chip->playback_substream = substream;
		else
			chip->capture_substream = substream;

		/* Use timer only if we didn't get an IRQ (fallback) */
		if (chip->irq == -1) {
			period_msec = (runtime->period_size * 1000) / runtime->rate;
			if (period_msec < 1)
				period_msec = 1;
			mod_timer(&qr->timer, jiffies + msecs_to_jiffies(period_msec));
		}
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (dump_on_trigger) {
			dev_info(&chip->pci->dev, "MMIO at trigger STOP:");
			for (i = 0; i < 64; i += 4)
				dev_info(&chip->pci->dev, "MMIO+0x%02x: 0x%08x\n", i, readl(chip->iobase + i));
		}

		/* Stop hardware stream */
		control_val = readl(chip->iobase + QUANTUM_REG_CONTROL);
		wmb();
		writel(0x0, chip->iobase + QUANTUM_REG_CONTROL);
		dev_info(&chip->pci->dev, "Trigger STOP %s: CONTROL 0x100 was 0x%x now 0x0\n",
			 substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
			 control_val);

		qr->running = false;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			chip->playback_substream = NULL;
		else
			chip->capture_substream = NULL;
		timer_delete_sync(&qr->timer);
		break;

	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/* Resume stream - same as START */
		{
			u32 ctrl = (control_value >= 0) ? (u32)control_value : 0x8u;
			control_val = readl(chip->iobase + QUANTUM_REG_CONTROL);
		    wmb();
			writel(ctrl, chip->iobase + QUANTUM_REG_CONTROL);
		}
		qr->running = true;
		if (chip->irq == -1) {
			period_msec = (runtime->period_size * 1000) / runtime->rate;
			if (period_msec < 1)
				period_msec = 1;
			mod_timer(&qr->timer, jiffies + msecs_to_jiffies(period_msec));
		}
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static snd_pcm_uframes_t quantum_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct quantum_chip *chip = substream->pcm->private_data;
	struct quantum_runtime *qr = substream->runtime->private_data;
	unsigned long flags;
	snd_pcm_uframes_t pos = 0;
	u32 hw_pos;

	if (!qr)
		return 0;

	/* Try to read hardware position from MMIO */
	/* Based on Ghidra: status registers may contain position info */
	if (chip->iobase) {
		/* Read status register that might contain position */
		/* This is a guess - actual position register needs experimentation */
		hw_pos = readl(chip->iobase + QUANTUM_REG_STATUS5);

		/* If hardware position is available, use it */
		/* For now, fall back to software position */
		spin_lock_irqsave(&qr->lock, flags);
		pos = qr->position;
		spin_unlock_irqrestore(&qr->lock, flags);
	} else {
		spin_lock_irqsave(&qr->lock, flags);
		pos = qr->position;
		spin_unlock_irqrestore(&qr->lock, flags);
	}

	return pos;
}

static const struct snd_pcm_ops quantum_pcm_ops = {
	.open = quantum_pcm_open,
	.close = quantum_pcm_close,
	.hw_params = quantum_pcm_hw_params,
	.hw_free = quantum_pcm_hw_free,
	.prepare = quantum_pcm_prepare,
	.trigger = quantum_pcm_trigger,
	.pointer = quantum_pcm_pointer,
};

static int snd_quantum_pcm_new(struct quantum_chip *chip)
{
	struct snd_pcm *pcm;
	int err;

	err = snd_pcm_new(chip->card, chip->model_name, 0, 1, 1, &pcm);
	if (err < 0)
		return err;
	pcm->private_data = chip;
	strscpy(pcm->name, chip->model_name, sizeof(pcm->name));
	chip->pcm = pcm;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &quantum_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &quantum_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, &chip->pci->dev, 64 * 1024, 256 * 1024);
	return 0;
}

/* ----- Resource release ----- */

static void snd_quantum_free(struct snd_card *card)
{
	struct quantum_chip *chip = card->private_data;

	if (chip->irq >= 0) {
		free_irq(chip->irq, chip);
		chip->irq = -1;
	}
	if (chip->msi_allocated) {
		pci_free_irq_vectors(chip->pci);
		chip->msi_allocated = false;
	}
	if (chip->iobase)
		pci_iounmap(chip->pci, chip->iobase);
	pci_release_regions(chip->pci);
	pci_disable_device(chip->pci);
}

/* ----- Interrupt: signal period elapsed for active substreams ----- */

static irqreturn_t snd_quantum_interrupt(int irq, void *dev_id)
{
	struct quantum_chip *chip = dev_id;
	struct snd_pcm_substream *s;
	struct quantum_runtime *qr;
	unsigned long flags;
	snd_pcm_uframes_t period_frames, buf_frames;
	u32 status;
	irqreturn_t handled = IRQ_NONE;

	dev_info(&chip->pci->dev, "Interrupt triggered (IRQ: %i)\n", irq);

	if (!chip->iobase)
		return IRQ_NONE;

	if (chip->disconnected)
		return IRQ_NONE;

	/* Read interrupt status register */
	/* Based on Ghidra: status registers at 0x4, 0x8, 0x10, 0x14, 0x104 */
	/* Check which one is the interrupt status (needs experimentation) */
	status = readl(chip->iobase + QUANTUM_REG_STATUS1);

	// 0xFFFFFFFF is returned when the bus is dead
	if (status == 0xffffffff) {
		dev_warn_ratelimited(&chip->pci->dev, "MMIO read failed, device disconnected?\n");
		chip->disconnected = true;
		return IRQ_NONE;
	}

	/* If no interrupt pending, return */
	/* For now, assume any non-zero status means interrupt */
	if (status == 0)
		return IRQ_NONE;

	/* Acknowledge interrupt (write to status register to clear) */
	/* Exact acknowledgment method needs experimentation */
	wmb();
	writel(status, chip->iobase + QUANTUM_REG_STATUS1);

	/* Handle playback */
	s = chip->playback_substream;
	if (s && snd_pcm_running(s)) {
		qr = s->runtime->private_data;
		if (qr) {
			period_frames = s->runtime->period_size;
			buf_frames = s->runtime->buffer_size;
			spin_lock_irqsave(&qr->lock, flags);
			qr->position += period_frames;
			if (qr->position >= buf_frames)
				qr->position -= buf_frames;
			spin_unlock_irqrestore(&qr->lock, flags);
			snd_pcm_period_elapsed(s);
			handled = IRQ_HANDLED;
		}
	}

	/* Handle capture */
	s = chip->capture_substream;
	if (s && snd_pcm_running(s)) {
		qr = s->runtime->private_data;
		if (qr) {
			period_frames = s->runtime->period_size;
			buf_frames = s->runtime->buffer_size;
			spin_lock_irqsave(&qr->lock, flags);
			qr->position += period_frames;
			if (qr->position >= buf_frames)
				qr->position -= buf_frames;
			spin_unlock_irqrestore(&qr->lock, flags);
			snd_pcm_period_elapsed(s);
			handled = IRQ_HANDLED;
		}
	}

	return handled;
}

/* ==========================================================================
 * FIRMWARE MAILBOX INTERFACE - SKELETON
 * TODO: Implement logic based on Ghidra analysis of QuantumSendFirmwareCommand
 * ========================================================================== */

/**
 * Send a command code to the firmware
 * @chip: The quantum chip structure
 * @cmd: The command byte (e.g., 0x39 for '9')
 * @data: Optional data buffer to send (can be NULL)
 * @data_len: Length of optional data
 *
 * Returns: 0 on success, negative error code otherwise.
 */
static int send_fw_cmd(struct quantum_chip *chip, u8 cmd, const u8 *data, size_t data_len)
{
	/* FIXME:
	 * 1. Identify the MMIO register to write the 'cmd' byte.
	 * 2. Identify if 'data' needs to be copied to a specific buffer/region first.
	 * 3. Identify the "Doorbell" register to trigger the command.
	 * 4. Implement the polling loop waiting for the "Done" status.
	 * 5. Handle timeouts and error flags.
	 */
	dev_err(&chip->pci->dev, "FIXME: Implement quantum_send_firmware_cmd (cmd=0x%02x)\n", cmd);
	return -ENOSYS;
}

/**
 * Read the response code and optional payload
 * @chip: The quantum chip structure
 * @response_code: Pointer to store the response byte (e.g., 0x3A for ':')
 * @buffer: Buffer to store extended response data (can be NULL)
 * @buffer_size: Size of the buffer
 *
 * Returns: 0 on success, negative error code otherwise.
 */
static int rcv_fw_resp(struct quantum_chip *chip, u8 *response_code, u8 *buffer, size_t buffer_size)
{
	/* FIXME:
	 * 1. Identify the MMIO register or memory region containing the response code.
	 * 2. Identify where the extended payload (e.g., channel list for cmd '9') is stored.
	 * 3. Copy the response data to 'buffer' up to 'buffer_size'.
	 * 4. Clear any "Done" or "Ack" flags if required by hardware.
	 */
	dev_err(&chip->pci->dev, "FIXME: Implement quantum_receive_firmware_response\n");
	return -ENOSYS;
}

/* ----- Device init (Windows FUN_140003d60 style: init reads + enable) ----- */

static int quantum_device_init(struct quantum_chip *chip)
{
    void __iomem *iobase = chip->iobase;
    u8 response_code;
    u8 channel_list[32]; // Buffer pour la liste des canaux
    int err;

    dev_info(&chip->pci->dev, "Starting Quantum Firmware Handshake...\n");

    // 1. Reset DMA/FIFO
	wmb();
    writel(0x00000000, iobase + QUANTUM_REG_DMA_RESET); // 0x11004

	// FIXME: probably not needed
    usleep_range(5000, 10000);

    // 2. Enumerate channels
    // Command'9' (0x39)
    err = send_fw_cmd(chip, 0x39, NULL, 0);
    if (err < 0) goto err_handshake;

    // Await response ':' (0x3A) and read channels list
    err = rcv_fw_resp(chip, &response_code, channel_list, sizeof(channel_list));
    if (err < 0 || response_code != 0x3A) {
        dev_err(&chip->pci->dev, "Failed Enumerate Channels: Got 0x%02x expected 0x3A\n", response_code);
        goto err_handshake;
    }

    // TODO: Parse channel_list and store to chip->active_channels_count
    dev_info(&chip->pci->dev, "Channels enumerated successfully.\n");

    // 3. Request topology
    // Command '3' (0x33)
    err = send_fw_cmd(chip, 0x33, NULL, 0);
    if (err < 0) goto err_handshake;

	// Await response '6' (0x36) and store topology
    err = rcv_fw_resp(chip, &response_code, NULL, 0);
    if (err < 0 || response_code != 0x36) {
        dev_err(&chip->pci->dev, "Failed Topology Handshake: Got 0x%02x\n", response_code);
        goto err_handshake;
    }

    // TODO: extract ChainID from response and store somewhere
	dev_info(&chip->pci->dev, "Topology obtained successfully.\n");

    // 4. Sync Sample Rate
    // Command '1' (0x31)
    err = send_fw_cmd(chip, 0x31, NULL, 0);
    if (err < 0) goto err_handshake;

	// Await response '5' (0x35) and inform ALSA
    err = rcv_fw_resp(chip, &response_code, NULL, 0);
    if (err < 0 || response_code != 0x35) { // Réponse attendue '5'
        dev_err(&chip->pci->dev, "Failed SampleRate Handshake: Got 0x%02x\n", response_code);
        goto err_handshake;
    }

	// TODO: decode sample rate & inform ALSA
	dev_info(&chip->pci->dev, "Initial sample rate obtained successfully.\n");


    // 5. FINAL GO (Synchronous for bring-up. Must be asynchronous to handle the soft power button.)
    // Command ';' (0x3B)
    err = send_fw_cmd(chip, 0x3B, NULL, 0);
    if (err < 0) goto err_handshake;

	// Await response '<' (0x3C)
    err = rcv_fw_resp(chip, &response_code, NULL, 0);
    if (err < 0 || response_code != 0x3C) {
        dev_err(&chip->pci->dev, "Failed Final Go Handshake: Got 0x%02x\n", response_code);
        goto err_handshake;
    }

	// TODO Verify response payload is '1'
    // If '0', the card refuses to start (Soft-Power Off?)
    dev_info(&chip->pci->dev, "Firmware Handshake SUCCESS. Interface ready.\n");

    // Now we may read & write to MMIO if needed
    // wmb();
    // writel(0x8, iobase + QUANTUM_REG_CONTROL);

    return 0;

err_handshake:
    dev_err(&chip->pci->dev, "Firmware Handshake FAILED. Device will not function.\n");
    return -EIO; // Or -ENODEV
}

/* ----- Create chip: enable PCI, claim BAR, IRQ, MMIO probe ----- */

static int snd_quantum_create(struct snd_card *card, struct pci_dev *pci)
{
	struct quantum_chip *chip = card->private_data;
	int err;

	chip->card = card;
	chip->pci = pci;
	chip->irq = -1;
	chip->msi_allocated = false;
	chip->playback_substream = NULL;
	chip->capture_substream = NULL;
	chip->playback_dma_addr = 0;
	chip->capture_dma_addr = 0;
	chip->playback_buffer_size = 0;
	chip->capture_buffer_size = 0;

	// TODO: Check we're not already initialized

	// TODO: Internal control channel

	// TODO: Internal control Request

	/* Initialize model-specific data (name, etc.) */
	quantum_init_model_data(chip, pci);

	// TODO: Map DeviceProfile

	dev_info(&pci->dev, "Enabling PCI device");
	err = pci_enable_device(pci);
	if (err < 0)
		return err;

	dev_info(&pci->dev, "Requesting PCI regions");
	err = pci_request_regions(pci, DRV_NAME);
	if (err < 0) {
		pci_disable_device(pci);
		return err;
	}

	dev_info(&pci->dev, "Preparing MMIO");
	chip->iobase = pci_iomap(pci, 0, 0);
	if (!chip->iobase) {
		err = -ENOMEM;
		goto fail_regions;
	}

	// 0. Read state
	dev_info(&pci->dev, "Red register 0x00: 0x%08x\n", readl(chip->iobase + 0x00));
	dev_info(&pci->dev, "Red register 0x04: 0x%08x\n", readl(chip->iobase + 0x04));
	dev_info(&pci->dev, "Red register 0x08: 0x%08x\n", readl(chip->iobase + 0x08));
	dev_info(&pci->dev, "Red register 0x10: 0x%08x\n", readl(chip->iobase + 0x10));
	dev_info(&pci->dev, "Red register 0x14: 0x%08x\n", readl(chip->iobase + 0x14));
	dev_info(&pci->dev, "Red register 0x10300 (VendorID): 0x%08x\n", readl(chip->iobase + 0x10300));
	dev_info(&pci->dev, "Red register 0x10304 (DeviceID): 0x%08x\n", readl(chip->iobase + 0x10304));
	dev_info(&pci->dev, "Red register 0x105 (FWVersion?): 0x%08x\n", readl(chip->iobase + 0x105));

	// TODO: initialize DMA and ranges
	// dev_info(&pci->dev, "Setting up DMA");
	// if (dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32)) < 0) {
	// 	pci_disable_device(pci);
	// 	return -ENXIO;
	// }
	//
	// /* Register access for reverse engineering */
	// if (reg_scan) {
	// 	dev_info(&pci->dev, "=== MMIO Scan (0x00-0xff) ===");
	// 	for (i = 0; i < 256; i += 4)
	// 		dev_info(&pci->dev, "MMIO+0x%02x: 0x%08x", i, readl(chip->iobase + i));
	// 	reg_scan = false; /* Clear after one scan */
	// }
	//
	// if (reg_read_offset >= 0 && reg_read_offset < (1024 * 1024)) {
	// 	u32 val = readl(chip->iobase + reg_read_offset);
	// 	dev_info(&pci->dev, "MMIO+0x%03x READ: 0x%08x", reg_read_offset, val);
	// 	reg_read_offset = -1; /* Clear after read */
	// }
	//
	// if (reg_write_offset >= 0 && reg_write_offset < (1024 * 1024)) {
	//  wmb();
	// 	writel(reg_write_value, chip->iobase + reg_write_offset);
	// 	dev_info(&pci->dev, "MMIO+0x%03x WRITE: 0x%08x (old: 0x%08x)",
	// 		 reg_write_offset, reg_write_value,
	// 		 readl(chip->iobase + reg_write_offset));
	// 	reg_write_offset = -1; /* Clear after write */
	// 	reg_write_value = 0;
	// }

	/* Attempt to allocate one MSI vector */
	if (pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_MSI) < 0) {
		dev_warn(&pci->dev, "MSI allocation failed. Falling back to timer-only mode (no IRQs).\n");
	} else {
		chip->irq = pci_irq_vector(pci, 0);
		chip->msi_allocated = true;

		/* Request the IRQ */
		if (request_irq(chip->irq, snd_quantum_interrupt, 0, DRV_NAME, chip) < 0) {
			dev_warn(&pci->dev, "IRQ request failed. Disabling MSI and falling back to timer-only mode.\n");
			pci_free_irq_vectors(pci);
			chip->irq = -1;
			chip->msi_allocated = false;
		} else {
			dev_info(&pci->dev, "MSI IRQ %d successfully allocated.\n", chip->irq);
			card->sync_irq = chip->irq;
		}
	}

	// TODO: Prepare workers or Linux equivalents

	dev_info(&pci->dev, "Initializing device");
	quantum_device_init(chip);

	// TODO: Launch workers

	err = snd_quantum_pcm_new(chip);
	if (err < 0)
		goto fail_pcm;

	return 0;

fail_pcm:
	if (chip->irq >= 0) {
		free_irq(chip->irq, chip);
		chip->irq = -1;
	}
	if (chip->msi_allocated) {
		pci_free_irq_vectors(pci);
		chip->msi_allocated = false;
	}
	pci_iounmap(pci, chip->iobase);
	chip->iobase = NULL;
fail_regions:
	pci_release_regions(pci);
	pci_disable_device(pci);
	return err;
}

/* ----- PCI table (from pae_quantum.inf) ----- */

static const struct pci_device_id snd_quantum_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_PRESONUS, PCI_DEVICE_ID_QUANTUM) },
	{ PCI_DEVICE(PCI_VENDOR_ID_PRESONUS, PCI_DEVICE_ID_QUANTUM2) },
	{ PCI_DEVICE(PCI_VENDOR_ID_PRESONUS, PCI_DEVICE_ID_QUANTUM4848) },
	{ PCI_DEVICE(PCI_VENDOR_ID_PRESONUS, PCI_DEVICE_ID_QUANTUM2626) },
	{ PCI_DEVICE(PCI_VENDOR_ID_PRESONUS, PCI_DEVICE_ID_QUANTUM_MOBILE) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, snd_quantum_ids);

/* ----- Probe / remove ----- */

static int snd_quantum_probe(struct pci_dev *pci, const struct pci_device_id *pci_id)
{
	static int dev;
	struct snd_card *card;
	struct quantum_chip *chip;
	int err;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}

	err = snd_devm_card_new(&pci->dev, index[dev], id[dev], THIS_MODULE,
				sizeof(struct quantum_chip), &card);
	if (err < 0)
		return err;
	chip = card->private_data;
	chip->irq = -1;

	card->private_free = snd_quantum_free;

	err = snd_quantum_create(card, pci);
	if (err < 0)
		return err;

	strscpy(card->driver, DRV_NAME, sizeof(card->driver));
	strscpy(card->shortname, chip->model_name, sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "%s at 0x%px irq %i",
		 chip->model_name, chip->iobase, chip->pci->irq);

	err = snd_card_register(card);
	if (err < 0)
		return err;

	pci_set_drvdata(pci, card);
	dev++;
	return 0;
}

static void snd_quantum_remove(struct pci_dev *pci)
{
	struct snd_card *card = pci_get_drvdata(pci);
	struct quantum_chip *chip;

	if (!card)
		return;

	chip = card->private_data;

	/* 1. Mark disconnected to block IRQ and timers */
	chip->disconnected = true;

	/* 2. Stop hardware before freing it */
	if (chip->iobase) {
		/* Write 0 to CONTROL to immediately stop DMA */
		wmb();
		writel(0x0, chip->iobase + QUANTUM_REG_CONTROL);
	}

	/* 3. Stop software timers */
	if (chip->playback_substream) {
		struct quantum_runtime *qr = chip->playback_substream->runtime->private_data;
		if (qr) {
			qr->running = false;
			timer_delete_sync(&qr->timer);
		}
	}
	if (chip->capture_substream) {
		struct quantum_runtime *qr = chip->capture_substream->runtime->private_data;
		if (qr) {
			qr->running = false;
			timer_delete_sync(&qr->timer);
		}
	}

	/* 4. Free ALSA resources */
	snd_card_free(card);

	/* Note: snd_card_free calls card->private_free (snd_quantum_free)
	   which will do pci_iounmap and free_irq. */

	dev_info(&pci->dev, "Removed driver successfully…\n");
}

static struct pci_driver quantum_driver = {
	.name = DRV_NAME,
	.id_table = snd_quantum_ids,
	.probe = snd_quantum_probe,
	.remove = snd_quantum_remove,
};

module_pci_driver(quantum_driver);
