/*-
 * Copyright (c) 2004, 2005 Philip Paeps <philip@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/dev/acpi_support/acpi_asus.c,v 1.24.2.4.2.1 2008/10/02 02:57:24 kensmith Exp $
 */

#include <sys/cdefs.h>

/*
 * Driver for extra ACPI-controlled gadgets (hotkeys, leds, etc) found on
 * recent Asus (and Medion) laptops.  Inspired by the acpi4asus project which
 * implements these features in the Linux kernel.
 *
 *   <http://sourceforge.net/projects/acpi4asus/>
 *
 * Currently should support most features, but could use some more testing.
 * Particularly the display-switching stuff is a bit hairy.  If you have an
 * Asus laptop which doesn't appear to be supported, or strange things happen
 * when using this driver, please report to <acpi@FreeBSD.org>.
 */

#include "opt_acpi.h"
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <machine/cpufunc.h>
#include <sys/module.h>
#include <sys/sensors.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/lock.h>
#include <sys/sbuf.h>
#include <sys/thread2.h>
#include <machine/clock.h>

#include "acpi.h"
#include "acpivar.h"
#include "acpi_if.h"

/* Methods */
#define ACPI_ASUS_METHOD_BRN	1
#define ACPI_ASUS_METHOD_DISP	2
#define ACPI_ASUS_METHOD_LCD	3

#define _COMPONENT	ACPI_OEM
ACPI_MODULE_NAME("ASUS")

struct acpi_asus_model {
	char	*name;

	char	*bled_set;
	char	*mled_set;
	char	*tled_set;
	char	*wled_set;

	char	*brn_get;
	char	*brn_set;
	char	*brn_up;
	char	*brn_dn;

	char	*lcd_get;
	char	*lcd_set;

	char	*disp_get;
	char	*disp_set;
};

struct acpi_asus_led {
	struct acpi_asus_softc *sc;
	struct cdev	*cdev;
	int		busy;
	int		state;
	enum {
		ACPI_ASUS_LED_BLED,
		ACPI_ASUS_LED_MLED,
		ACPI_ASUS_LED_TLED,
		ACPI_ASUS_LED_WLED,
	} type;
};

struct acpi_asus_softc {
	device_t		dev;
	ACPI_HANDLE		handle;

	struct acpi_asus_model	*model;
	struct sysctl_ctx_list	sysctl_ctx;
	struct sysctl_oid	*sysctl_tree;

	struct acpi_asus_led	s_bled;
	struct acpi_asus_led	s_mled;
	struct acpi_asus_led	s_tled;
	struct acpi_asus_led	s_wled;

	int			s_brn;
	int			s_disp;
	int			s_lcd;
};

/*
 * We can identify Asus laptops from the string they return
 * as a result of calling the ATK0100 'INIT' method.
 */
static struct acpi_asus_model acpi_asus_models[] = {
	{
		.name		= "xxN",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.lcd_get	= "\\BKLT",
		.lcd_set	= "\\_SB.PCI0.SBRG.EC0._Q10",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.disp_get	= "\\ADVG",
		.disp_set	= "SDSP"
	},
	{
		.name		= "A1x",
		.mled_set	= "MLED",
		.lcd_get	= "\\BKLI",
		.lcd_set	= "\\_SB.PCI0.ISA.EC0._Q10",
		.brn_up		= "\\_SB.PCI0.ISA.EC0._Q0E",
		.brn_dn		= "\\_SB.PCI0.ISA.EC0._Q0F"
	},
	{
		.name		= "A2x",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.lcd_get	= "\\BAOF",
		.lcd_set	= "\\Q10",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.disp_get	= "\\INFB",
		.disp_set	= "SDSP"
	},
	{
		.name           = "A3N",
		.mled_set       = "MLED",
		.bled_set       = "BLED",
		.wled_set       = "WLED",
		.lcd_get        = NULL,
		.lcd_set        = "\\_SB.PCI0.SBRG.EC0._Q10",
		.brn_set        = "SPLV",
		.brn_get        = "SDSP",
		.disp_set       = "SDSP",
		.disp_get       = "\\_SB.PCI0.P0P3.VGA.GETD"
	},
	{
		.name		= "A4D",
		.mled_set	= "MLED",
		.brn_up		= "\\_SB_.PCI0.SBRG.EC0._Q0E",
		.brn_dn		= "\\_SB_.PCI0.SBRG.EC0._Q0F",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
#ifdef notyet
		.disp_get	= "\\_SB_.PCI0.SBRG.EC0._Q10",
		.disp_set	= "\\_SB_.PCI0.SBRG.EC0._Q11"
#endif
	},
	{
		.name		= "A6V",
		.bled_set	= "BLED",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.lcd_get	= NULL,
		.lcd_set	= "\\_SB.PCI0.SBRG.EC0._Q10",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.disp_get	= "\\_SB.PCI0.P0P3.VGA.GETD",
		.disp_set	= "SDSP"
	},
	{
		.name		= "D1x",
		.mled_set	= "MLED",
		.lcd_get	= "\\GP11",
		.lcd_set	= "\\Q0D",
		.brn_up		= "\\Q0C",
		.brn_dn		= "\\Q0B",
		.disp_get	= "\\INFB",
		.disp_set	= "SDSP"
	},
	{
		.name		= "L2D",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.brn_up		= "\\Q0E",
		.brn_dn		= "\\Q0F",
		.lcd_get	= "\\SGP0",
		.lcd_set	= "\\Q10"
	},
	{
		.name		= "L3C",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.lcd_get	= "\\GL32",
		.lcd_set	= "\\_SB.PCI0.PX40.ECD0._Q10"
	},
	{
		.name		= "L3D",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.lcd_get	= "\\BKLG",
		.lcd_set	= "\\Q10"
	},
	{
		.name		= "L3H",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.lcd_get	= "\\_SB.PCI0.PM.PBC",
		.lcd_set	= "EHK",
		.disp_get	= "\\_SB.INFB",
		.disp_set	= "SDSP"
	},
	{
		.name		= "L4R",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.lcd_get	= "\\_SB.PCI0.SBSM.SEO4",
		.lcd_set	= "\\_SB.PCI0.SBRG.EC0._Q10",
		.disp_get	= "\\_SB.PCI0.P0P1.VGA.GETD",
		.disp_set	= "SDSP"
	},
	{
		.name		= "L5x",
		.mled_set	= "MLED",
		.tled_set	= "TLED",
		.lcd_get	= "\\BAOF",
		.lcd_set	= "\\Q0D",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.disp_get	= "\\INFB",
		.disp_set	= "SDSP"
	},
	{
		.name		= "L8L"
		/* Only has hotkeys, apparantly */
	},
	{
		.name		= "M1A",
		.mled_set	= "MLED",
		.brn_up		= "\\_SB.PCI0.PX40.EC0.Q0E",
		.brn_dn		= "\\_SB.PCI0.PX40.EC0.Q0F",
		.lcd_get	= "\\PNOF",
		.lcd_set	= "\\_SB.PCI0.PX40.EC0.Q10"
	},
	{
		.name		= "M2E",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.lcd_get	= "\\GP06",
		.lcd_set	= "\\Q10"
	},
	{
		.name		= "M6N",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.lcd_set	= "\\_SB.PCI0.SBRG.EC0._Q10",
		.lcd_get	= "\\_SB.BKLT",
		.brn_set	= "SPLV",
		.brn_get	= "GPLV",
		.disp_set	= "SDSP",
		.disp_get	= "\\SSTE"
	},
	{
		.name		= "M6R",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.lcd_get	= "\\_SB.PCI0.SBSM.SEO4",
		.lcd_set	= "\\_SB.PCI0.SBRG.EC0._Q10",
		.disp_get	= "\\SSTE",
		.disp_set	= "SDSP"
	},
	{
		.name		= "S1x",
		.mled_set	= "MLED",
		.wled_set	= "WLED",
		.lcd_get	= "\\PNOF",
		.lcd_set	= "\\_SB.PCI0.PX40.Q10",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV"
	},
	{
		.name		= "S2x",
		.mled_set	= "MLED",
		.lcd_get	= "\\BKLI",
		.lcd_set	= "\\_SB.PCI0.ISA.EC0._Q10",
		.brn_up		= "\\_SB.PCI0.ISA.EC0._Q0B",
		.brn_dn		= "\\_SB.PCI0.ISA.EC0._Q0A"
	},
	{
		.name		= "V6V",
		.bled_set	= "BLED",
		.tled_set	= "TLED",
		.wled_set	= "WLED",
		.lcd_get	= "\\BKLT",
		.lcd_set	= "\\_SB.PCI0.SBRG.EC0._Q10",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.disp_get	= "\\_SB.PCI0.P0P1.VGA.GETD",
		.disp_set	= "SDSP"
	},
	{
		.name		= "W5A",
		.bled_set	= "BLED",
		.lcd_get	= "\\BKLT",
		.lcd_set	= "\\_SB.PCI0.SBRG.EC0._Q10",
		.brn_get	= "GPLV",
		.brn_set	= "SPLV",
		.disp_get	= "\\_SB.PCI0.P0P2.VGA.GETD",
		.disp_set	= "SDSP"
	},

	{ .name = NULL }
};

/*
 * Samsung P30/P35 laptops have an Asus ATK0100 gadget interface,
 * but they can't be probed quite the same way as Asus laptops.
 */
static struct acpi_asus_model acpi_samsung_models[] = {
	{
		.name		= "P30",
		.wled_set	= "WLED",
		.brn_up		= "\\_SB.PCI0.LPCB.EC0._Q68",
		.brn_dn		= "\\_SB.PCI0.LPCB.EC0._Q69",
		.lcd_get	= "\\BKLT",
		.lcd_set	= "\\_SB.PCI0.LPCB.EC0._Q0E"
	},

	{ .name = NULL }
};

/*
 * EeePC have an Asus ASUS010 gadget interface,
 * but they can't be probed quite the same way as Asus laptops.
 */
static struct acpi_asus_model acpi_eeepc_models[] = {
	{
		.name		= "EEE",
		.brn_get	= "\\_SB.ATKD.PBLG",
		.brn_set	= "\\_SB.ATKD.PBLS"
	},

	{ .name = NULL }
};

static struct {
	char	*name;
	char	*description;
	int	method;
} acpi_asus_sysctls[] = {
	{
		.name		= "lcd_backlight",
		.method		= ACPI_ASUS_METHOD_LCD,
		.description	= "state of the lcd backlight"
	},
	{
		.name		= "lcd_brightness",
		.method		= ACPI_ASUS_METHOD_BRN,
		.description	= "brightness of the lcd panel"
	},
	{
		.name		= "video_output",
		.method		= ACPI_ASUS_METHOD_DISP,
		.description	= "display output state"
	},

	{ .name = NULL }
};

static int	acpi_asus_probe(device_t dev);
static int	acpi_asus_attach(device_t dev);
static int	acpi_asus_detach(device_t dev);

static struct lock asuslock;

static int	acpi_asus_sysctl(SYSCTL_HANDLER_ARGS);
static int	acpi_asus_sysctl_init(struct acpi_asus_softc *sc, int method);
static int	acpi_asus_sysctl_get(struct acpi_asus_softc *sc, int method);
static int	acpi_asus_sysctl_set(struct acpi_asus_softc *sc, int method, int val);

static void	acpi_asus_notify(ACPI_HANDLE h, UINT32 notify, void *context);

static device_method_t acpi_asus_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe, acpi_asus_probe),
	DEVMETHOD(device_attach, acpi_asus_attach),
	DEVMETHOD(device_detach, acpi_asus_detach),
	{0, 0}
};

static driver_t	acpi_asus_driver = {
	"acpi_asus",
	acpi_asus_methods,
	sizeof(struct acpi_asus_softc),
};

static devclass_t acpi_asus_devclass;

DRIVER_MODULE(acpi_asus, acpi, acpi_asus_driver,
    acpi_asus_devclass, 0, 0);
MODULE_DEPEND(acpi_asus, acpi, 1, 1, 1);

static char		*asus_ids[] = { "ATK0100", "ASUS010", NULL };

static int
acpi_asus_probe(device_t dev)
{
	struct acpi_asus_model	*model;
	struct acpi_asus_softc	*sc;
	struct sbuf		*sb;
	ACPI_BUFFER		Buf;
	ACPI_OBJECT		Arg, *Obj;
	ACPI_OBJECT_LIST	Args;
	char *rstr;

	ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

	if (acpi_disabled("asus")) return (ENXIO);
	rstr = ACPI_ID_PROBE(device_get_parent(dev), dev, asus_ids);
	if (rstr == NULL) {
		return (ENXIO);
	}


	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->handle = acpi_get_handle(dev);


	Arg.Type = ACPI_TYPE_INTEGER;
	Arg.Integer.Value = 0;

	Args.Count = 1;
	Args.Pointer = &Arg;

	Buf.Pointer = NULL;
	Buf.Length = ACPI_ALLOCATE_BUFFER;

	AcpiEvaluateObject(sc->handle, "INIT", &Args, &Buf);
	Obj = Buf.Pointer;

	/*
	 * The Samsung P30 returns a null-pointer from INIT, we
	 * can identify it from the 'ODEM' string in the DSDT.
	 */
	if (Obj->String.Pointer == NULL) {
		ACPI_STATUS		status;
		ACPI_TABLE_HEADER	th;

		status = AcpiGetTableHeader(ACPI_SIG_DSDT, 1, &th);
		if (ACPI_FAILURE(status)) {
			device_printf(dev, "Unsupported (Samsung?) laptop\n");
			AcpiOsFree(Buf.Pointer);
			return (ENXIO);
		}

		if (strncmp("ODEM", th.OemTableId, 4) == 0) {
			sc->model = &acpi_samsung_models[0];
			device_set_desc(dev, "Samsung P30 Laptop Extras");
			AcpiOsFree(Buf.Pointer);
			return (0);
		}

		/* if EeePC */
		if (strncmp("ASUS010", rstr, 7) == 0) {
			sc->model = &acpi_eeepc_models[0];
			device_set_desc(dev, "ASUS EeePC");
			AcpiOsFree(Buf.Pointer);
			return (0);
		}
	}

	sb = sbuf_new(NULL, NULL, 0, SBUF_AUTOEXTEND);
	if (sb == NULL)
		return (ENOMEM);

	/*
	 * Asus laptops are simply identified by name, easy!
	 */
	for (model = acpi_asus_models; model->name != NULL; model++) {
		if (strncmp(Obj->String.Pointer, model->name, 3) == 0) {

good:
			sbuf_printf(sb, "Asus %s Laptop Extras",
			    Obj->String.Pointer);
			sbuf_finish(sb);

			sc->model = model;
			device_set_desc_copy(dev, sbuf_data(sb));

			sbuf_delete(sb);
			AcpiOsFree(Buf.Pointer);
			return (0);
		}

		/*
		 * Some models look exactly the same as other models, but have
		 * their own ids.  If we spot these, set them up with the same
		 * details as the models they're like, possibly dealing with
		 * small differences.
		 *
		 * XXX: there must be a prettier way to do this!
		 */
		else if (strncmp(model->name, "xxN", 3) == 0 &&
		    (strncmp(Obj->String.Pointer, "M3N", 3) == 0 ||
		     strncmp(Obj->String.Pointer, "S1N", 3) == 0))
			goto good;
		else if (strncmp(model->name, "A1x", 3) == 0 &&
		    strncmp(Obj->String.Pointer, "A1", 2) == 0)
			goto good;
		else if (strncmp(model->name, "A2x", 3) == 0 &&
		    strncmp(Obj->String.Pointer, "A2", 2) == 0)
			goto good;
		else if (strncmp(model->name, "D1x", 3) == 0 &&
		    strncmp(Obj->String.Pointer, "D1", 2) == 0)
			goto good;
		else if (strncmp(model->name, "L3H", 3) == 0 &&
		    strncmp(Obj->String.Pointer, "L2E", 3) == 0)
			goto good;
		else if (strncmp(model->name, "L5x", 3) == 0 &&
		    strncmp(Obj->String.Pointer, "L5", 2) == 0)
			goto good;
		else if (strncmp(model->name, "M2E", 3) == 0 &&
		    (strncmp(Obj->String.Pointer, "M2", 2) == 0 ||
		     strncmp(Obj->String.Pointer, "L4E", 3) == 0))
			goto good;
		else if (strncmp(model->name, "S1x", 3) == 0 &&
		    (strncmp(Obj->String.Pointer, "L8", 2) == 0 ||
		     strncmp(Obj->String.Pointer, "S1", 2) == 0))
			goto good;
		else if (strncmp(model->name, "S2x", 3) == 0 &&
		    (strncmp(Obj->String.Pointer, "J1", 2) == 0 ||
		     strncmp(Obj->String.Pointer, "S2", 2) == 0))
			goto good;

		/* L2B is like L3C but has no lcd_get method */
		else if (strncmp(model->name, "L3C", 3) == 0 &&
		    strncmp(Obj->String.Pointer, "L2B", 3) == 0) {
			model->lcd_get = NULL;
			goto good;
		}

		/* A3G is like M6R but with a different lcd_get method */
		else if (strncmp(model->name, "M6R", 3) == 0 &&
		    strncmp(Obj->String.Pointer, "A3G", 3) == 0) {
			model->lcd_get = "\\BLFG";
			goto good;
		}

		/* M2N and W1N are like xxN with added WLED */
		else if (strncmp(model->name, "xxN", 3) == 0 &&
		    (strncmp(Obj->String.Pointer, "M2N", 3) == 0 ||
		     strncmp(Obj->String.Pointer, "W1N", 3) == 0)) {
			model->wled_set = "WLED";
			goto good;
		}

		/* M5N and S5N are like xxN without MLED */
		else if (strncmp(model->name, "xxN", 3) == 0 &&
		    (strncmp(Obj->String.Pointer, "M5N", 3) == 0 ||
		     strncmp(Obj->String.Pointer, "S5N", 3) == 0)) {
			model->mled_set = NULL;
			goto good;
		}
	}

	sbuf_printf(sb, "Unsupported Asus laptop: %s\n", Obj->String.Pointer);
	sbuf_finish(sb);

	device_printf(dev, sbuf_data(sb));

	sbuf_delete(sb);
	AcpiOsFree(Buf.Pointer);

	return (ENXIO);
}

static int
acpi_asus_detach(device_t dev)
{
	struct acpi_asus_softc	*sc;

	ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

	sc = device_get_softc(dev);


	/* Turn the lights off */
	/* We don't have LED support, sadly... :( */
/*	if (sc->model->bled_set)
		led_destroy(sc->s_bled.cdev);

	if (sc->model->mled_set)
		led_destroy(sc->s_mled.cdev);

	if (sc->model->tled_set)
		led_destroy(sc->s_tled.cdev);

	if (sc->model->wled_set)
		led_destroy(sc->s_wled.cdev);
*/
	/* Remove notify handler */
	AcpiRemoveNotifyHandler(sc->handle, ACPI_SYSTEM_NOTIFY,
	    acpi_asus_notify);

	/* Free sysctl tree */
	sysctl_ctx_free(&sc->sysctl_ctx);

	return (0);
}

static int
acpi_asus_attach(device_t dev)
{
	struct acpi_asus_softc	*sc;
	struct acpi_softc	*acpi_sc;

	ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

	sc = device_get_softc(dev);
	acpi_sc = acpi_device_get_parent_softc(dev);

	/* Build sysctl tree */
	sysctl_ctx_init(&sc->sysctl_ctx);
	sc->sysctl_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(acpi_sc->acpi_sysctl_tree),
	    OID_AUTO, "asus", CTLFLAG_RD, 0, "");

	/* Hook up nodes */
	for (int i = 0; acpi_asus_sysctls[i].name != NULL; i++) {
		if (!acpi_asus_sysctl_init(sc, acpi_asus_sysctls[i].method))
			continue;

		SYSCTL_ADD_PROC(&sc->sysctl_ctx,
		    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
		    acpi_asus_sysctls[i].name,
		    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY,
		    sc, i, acpi_asus_sysctl, "I",
		    acpi_asus_sysctls[i].description);
	}

	/* Attach leds */

   /*Currently we don't have LEDs control. Just comment this for now...*/
/*
	if (sc->model->bled_set) {
		sc->s_bled.busy = 0;
		sc->s_bled.sc = sc;
		sc->s_bled.type = ACPI_ASUS_LED_BLED;
		sc->s_bled.cdev =
		    led_create((led_t *)acpi_asus_led, &sc->s_bled, "bled");
	}

	if (sc->model->mled_set) {
		sc->s_mled.busy = 0;
		sc->s_mled.sc = sc;
		sc->s_mled.type = ACPI_ASUS_LED_MLED;
		sc->s_mled.cdev =
		    led_create((led_t *)acpi_asus_led, &sc->s_mled, "mled");
	}

	if (sc->model->tled_set) {
		sc->s_tled.busy = 0;
		sc->s_tled.sc = sc;
		sc->s_tled.type = ACPI_ASUS_LED_TLED;
		sc->s_tled.cdev =
		    led_create((led_t *)acpi_asus_led, &sc->s_tled, "tled");
	}

	if (sc->model->wled_set) {
		sc->s_wled.busy = 0;
		sc->s_wled.sc = sc;
		sc->s_wled.type = ACPI_ASUS_LED_WLED;
		sc->s_wled.cdev =
		    led_create((led_t *)acpi_asus_led, &sc->s_wled, "wled");
	}
*/

	/* Activate hotkeys */
	AcpiEvaluateObject(sc->handle, "BSTS", NULL, NULL);

	/* Handle notifies */
	AcpiInstallNotifyHandler(sc->handle, ACPI_SYSTEM_NOTIFY,
	    acpi_asus_notify, dev);

    lockinit(&asuslock, "asus", 0, 0);

	return (0);
}

static int
acpi_asus_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct acpi_asus_softc	*sc;
	int			arg;
	int			error = 0;
	int			function;
	int			method;

	ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

	sc = (struct acpi_asus_softc *)oidp->oid_arg1;
	function = oidp->oid_arg2;
	method = acpi_asus_sysctls[function].method;

	lockmgr(&asuslock, LK_EXCLUSIVE);
	arg = acpi_asus_sysctl_get(sc, method);
	error = sysctl_handle_int(oidp, &arg, 0, req);

	/* Sanity check */
	if (error != 0 || req->newptr == NULL)
		goto out;

	/* Update */
	error = acpi_asus_sysctl_set(sc, method, arg);

out:
	lockmgr(&asuslock, LK_RELEASE);
	return (error);
}

static int
acpi_asus_sysctl_get(struct acpi_asus_softc *sc, int method)
{
	int val = 0;

	ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);
	KKASSERT(lockstatus(&asuslock, NULL)!=0); /* was ACPI_SERIAL_ASSERT(asus); may be I lost something? */

	switch (method) {
	case ACPI_ASUS_METHOD_BRN:
		val = sc->s_brn;
		break;
	case ACPI_ASUS_METHOD_DISP:
		val = sc->s_disp;
		break;
	case ACPI_ASUS_METHOD_LCD:
		val = sc->s_lcd;
		break;
	}

	return (val);
}

static int
acpi_asus_sysctl_set(struct acpi_asus_softc *sc, int method, int arg)
{
	ACPI_STATUS	status = AE_OK;

	ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);


	KKASSERT(lockstatus(&asuslock, NULL)!=0); /* was ACPI_SERIAL_ASSERT(asus); another miss? */

	switch (method) {
	case ACPI_ASUS_METHOD_BRN:
		if (arg < 0 || arg > 15)
			return (EINVAL);

		if (sc->model->brn_set)
			status = acpi_SetInteger(sc->handle,
			    sc->model->brn_set, arg);
		else {
			while (arg != 0) {
				status = AcpiEvaluateObject(sc->handle,
				    (arg > 0) ?  sc->model->brn_up :
				    sc->model->brn_dn, NULL, NULL);
				(arg > 0) ? arg-- : arg++;
			}
		}

		if (ACPI_SUCCESS(status))
			sc->s_brn = arg;

		break;
	case ACPI_ASUS_METHOD_DISP:
		if (arg < 0 || arg > 7)
			return (EINVAL);

		status = acpi_SetInteger(sc->handle,
		    sc->model->disp_set, arg);

		if (ACPI_SUCCESS(status))
			sc->s_disp = arg;

		break;
	case ACPI_ASUS_METHOD_LCD:
		if (arg < 0 || arg > 1)
			return (EINVAL);

		if (strncmp(sc->model->name, "L3H", 3) != 0)
			status = AcpiEvaluateObject(sc->handle,
			    sc->model->lcd_set, NULL, NULL);
		else
			status = acpi_SetInteger(sc->handle,
			    sc->model->lcd_set, 0x7);

		if (ACPI_SUCCESS(status))
			sc->s_lcd = arg;

		break;
	}

	return (0);
}

static int
acpi_asus_sysctl_init(struct acpi_asus_softc *sc, int method)
{
	ACPI_STATUS	status;

	switch (method) {
	case ACPI_ASUS_METHOD_BRN:
		if (sc->model->brn_get) {
			/* GPLV/SPLV models */
			status = acpi_GetInteger(sc->handle,
			    sc->model->brn_get, &sc->s_brn);
			if (ACPI_SUCCESS(status))
				return (TRUE);
		} else if (sc->model->brn_up) {
			/* Relative models */
			status = AcpiEvaluateObject(sc->handle,
			    sc->model->brn_up, NULL, NULL);
			if (ACPI_FAILURE(status))
				return (FALSE);

			status = AcpiEvaluateObject(sc->handle,
			    sc->model->brn_dn, NULL, NULL);
			if (ACPI_FAILURE(status))
				return (FALSE);

			return (TRUE);
		}
		return (FALSE);
	case ACPI_ASUS_METHOD_DISP:
		if (sc->model->disp_get) {
			status = acpi_GetInteger(sc->handle,
			    sc->model->disp_get, &sc->s_disp);
			if (ACPI_SUCCESS(status))
				return (TRUE);
		}
		return (FALSE);
	case ACPI_ASUS_METHOD_LCD:
		if (sc->model->lcd_get &&
		    strncmp(sc->model->name, "L3H", 3) != 0) {
			status = acpi_GetInteger(sc->handle,
			    sc->model->lcd_get, &sc->s_lcd);
			if (ACPI_SUCCESS(status))
				return (TRUE);
		}
		else if (sc->model->lcd_get) {
			ACPI_BUFFER		Buf;
			ACPI_OBJECT		Arg[2], Obj;
			ACPI_OBJECT_LIST	Args;

			/* L3H is a bit special */
			Arg[0].Type = ACPI_TYPE_INTEGER;
			Arg[0].Integer.Value = 0x02;
			Arg[1].Type = ACPI_TYPE_INTEGER;
			Arg[1].Integer.Value = 0x03;

			Args.Count = 2;
			Args.Pointer = Arg;

			Buf.Length = sizeof(Obj);
			Buf.Pointer = &Obj;

			status = AcpiEvaluateObject(sc->handle,
			    sc->model->lcd_get, &Args, &Buf);
			if (ACPI_SUCCESS(status) &&
			    Obj.Type == ACPI_TYPE_INTEGER) {
				sc->s_lcd = Obj.Integer.Value >> 8;
				return (TRUE);
			}
		}
		return (FALSE);
	}
	return (FALSE);
}

static void
acpi_asus_notify(ACPI_HANDLE h, UINT32 notify, void *context)
{
	struct acpi_asus_softc	*sc;
	struct acpi_softc	*acpi_sc;

	ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

	sc = device_get_softc((device_t)context);
	acpi_sc = acpi_device_get_parent_softc(sc->dev);

	lockmgr(&asuslock, LK_EXCLUSIVE);
	if ((notify & ~0x10) <= 15) {
		sc->s_brn = notify & ~0x10;
		ACPI_VPRINT(sc->dev, acpi_sc, "Brightness increased\n");
	} else if ((notify & ~0x20) <= 15) {
		sc->s_brn = notify & ~0x20;
		ACPI_VPRINT(sc->dev, acpi_sc, "Brightness decreased\n");
	} else if (notify == 0x33) {
		sc->s_lcd = 1;
		ACPI_VPRINT(sc->dev, acpi_sc, "LCD turned on\n");
	} else if (notify == 0x34) {
		sc->s_lcd = 0;
		ACPI_VPRINT(sc->dev, acpi_sc, "LCD turned off\n");
	} else {
		/* Notify devd(8) */
		acpi_UserNotify("ASUS", h, notify);
	}
	lockmgr(&asuslock, LK_RELEASE);
}