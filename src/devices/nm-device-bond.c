/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright 2011 - 2016 Red Hat, Inc.
 */

#include "nm-default.h"

#include <errno.h>
#include <stdlib.h>

#include "nm-device-bond.h"
#include "NetworkManagerUtils.h"
#include "nm-device-private.h"
#include "nm-platform.h"
#include "nm-enum-types.h"
#include "nm-device-factory.h"
#include "nm-core-internal.h"
#include "nm-ip4-config.h"

#include "nmdbus-device-bond.h"

#include "nm-device-logging.h"
_LOG_DECLARE_SELF(NMDeviceBond);

G_DEFINE_TYPE (NMDeviceBond, nm_device_bond, NM_TYPE_DEVICE)

#define NM_DEVICE_BOND_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_DEVICE_BOND, NMDeviceBondPrivate))

typedef struct {
	int dummy;
} NMDeviceBondPrivate;

/******************************************************************/

static NMDeviceCapabilities
get_generic_capabilities (NMDevice *dev)
{
	return NM_DEVICE_CAP_CARRIER_DETECT | NM_DEVICE_CAP_IS_SOFTWARE;
}

static gboolean
is_available (NMDevice *dev, NMDeviceCheckDevAvailableFlags flags)
{
	return TRUE;
}

static gboolean
check_connection_available (NMDevice *device,
                            NMConnection *connection,
                            NMDeviceCheckConAvailableFlags flags,
                            const char *specific_object)
{
	/* Connections are always available because the carrier state is determined
	 * by the slave carrier states, not the bonds's state.
	 */
	return TRUE;
}

static gboolean
check_connection_compatible (NMDevice *device, NMConnection *connection)
{
	NMSettingBond *s_bond;

	if (!NM_DEVICE_CLASS (nm_device_bond_parent_class)->check_connection_compatible (device, connection))
		return FALSE;

	s_bond = nm_connection_get_setting_bond (connection);
	if (!s_bond || !nm_connection_is_type (connection, NM_SETTING_BOND_SETTING_NAME))
		return FALSE;

	/* FIXME: match bond properties like mode, etc? */

	return TRUE;
}

static gboolean
complete_connection (NMDevice *device,
                     NMConnection *connection,
                     const char *specific_object,
                     const GSList *existing_connections,
                     GError **error)
{
	NMSettingBond *s_bond;

	nm_utils_complete_generic (NM_PLATFORM_GET,
	                           connection,
	                           NM_SETTING_BOND_SETTING_NAME,
	                           existing_connections,
	                           NULL,
	                           _("Bond connection"),
	                           "bond",
	                           TRUE);

	s_bond = nm_connection_get_setting_bond (connection);
	if (!s_bond) {
		s_bond = (NMSettingBond *) nm_setting_bond_new ();
		nm_connection_add_setting (connection, NM_SETTING (s_bond));
	}

	return TRUE;
}

/******************************************************************/

static gboolean
set_bond_attr (NMDevice *device, NMBondMode mode, const char *attr, const char *value)
{
	NMDeviceBond *self = NM_DEVICE_BOND (device);
	gboolean ret;
	int ifindex = nm_device_get_ifindex (device);

	if (!_nm_setting_bond_option_supported (attr, mode))
		return FALSE;

	ret = nm_platform_sysctl_master_set_option (NM_PLATFORM_GET, ifindex, attr, value);
	if (!ret)
		_LOGW (LOGD_HW, "failed to set bonding attribute '%s' to '%s'", attr, value);
	return ret;
}

/* Ignore certain bond options if they are zero (off/disabled) */
static gboolean
ignore_if_zero (const char *option, const char *value)
{
	if (!NM_IN_STRSET (option, NM_SETTING_BOND_OPTION_ARP_INTERVAL,
	                           NM_SETTING_BOND_OPTION_DOWNDELAY,
	                           NM_SETTING_BOND_OPTION_MIIMON,
	                           NM_SETTING_BOND_OPTION_UPDELAY))
		return FALSE;

	return g_strcmp0 (value, "0") == 0 ? TRUE : FALSE;
}

static void
update_connection (NMDevice *device, NMConnection *connection)
{
	NMSettingBond *s_bond = nm_connection_get_setting_bond (connection);
	int ifindex = nm_device_get_ifindex (device);
	const char **options;

	if (!s_bond) {
		s_bond = (NMSettingBond *) nm_setting_bond_new ();
		nm_connection_add_setting (connection, (NMSetting *) s_bond);
	}

	/* Read bond options from sysfs and update the Bond setting to match */
	options = nm_setting_bond_get_valid_options (s_bond);
	while (options && *options) {
		gs_free char *value = nm_platform_sysctl_master_get_option (NM_PLATFORM_GET, ifindex, *options);
		const char *defvalue = nm_setting_bond_get_option_default (s_bond, *options);
		char *p;

		if (_nm_setting_bond_get_option_type (s_bond, *options) == NM_BOND_OPTION_TYPE_BOTH) {
			p = strchr (value, ' ');
			if (p)
				*p = '\0';
		}

		if (   value
		    && value[0]
		    && !ignore_if_zero (*options, value)
		    && !nm_streq0 (value, defvalue)) {
			/* Replace " " with "," for arp_ip_targets from the kernel */
			if (strcmp (*options, NM_SETTING_BOND_OPTION_ARP_IP_TARGET) == 0) {
				for (p = value; *p; p++) {
					if (*p == ' ')
						*p = ',';
				}
			}

			nm_setting_bond_add_option (s_bond, *options, value);
		}
		options++;
	}
}

static gboolean
master_update_slave_connection (NMDevice *self,
                                NMDevice *slave,
                                NMConnection *connection,
                                GError **error)
{
	g_object_set (nm_connection_get_setting_connection (connection),
	              NM_SETTING_CONNECTION_MASTER, nm_device_get_iface (self),
	              NM_SETTING_CONNECTION_SLAVE_TYPE, NM_SETTING_BOND_SETTING_NAME,
	              NULL);
	return TRUE;
}

static void
set_arp_targets (NMDevice *device,
                 NMBondMode mode,
                 const char *value,
                 const char *delim,
                 const char *prefix)
{
	char **items, **iter, *tmp;

	if (!value || !*value)
		return;

	items = g_strsplit_set (value, delim, 0);
	for (iter = items; iter && *iter; iter++) {
		if (*iter[0]) {
			tmp = g_strdup_printf ("%s%s", prefix, *iter);
			set_bond_attr (device, mode, NM_SETTING_BOND_OPTION_ARP_IP_TARGET, tmp);
			g_free (tmp);
		}
	}
	g_strfreev (items);
}

static void
set_simple_option (NMDevice *device,
                   NMBondMode mode,
                   NMSettingBond *s_bond,
                   const char *opt)
{
	const char *value;

	value = nm_setting_bond_get_option_by_name (s_bond, opt);
	if (!value)
		value = nm_setting_bond_get_option_default (s_bond, opt);
	set_bond_attr (device, mode, opt, value);
}

static NMActStageReturn
apply_bonding_config (NMDevice *device)
{
	NMDeviceBond *self = NM_DEVICE_BOND (device);
	NMConnection *connection;
	NMSettingBond *s_bond;
	int ifindex = nm_device_get_ifindex (device);
	const char *mode_str, *value;
	char *contents;
	gboolean set_arp_interval = TRUE;
	NMBondMode mode;

	/* Option restrictions:
	 *
	 * arp_interval conflicts miimon > 0
	 * arp_interval conflicts [ alb, tlb ]
	 * arp_validate needs [ active-backup ]
	 * downdelay needs miimon
	 * updelay needs miimon
	 * primary needs [ active-backup, tlb, alb ]
	 *
	 * clearing miimon requires that arp_interval be 0, but clearing
	 *     arp_interval doesn't require miimon to be 0
	 */

	connection = nm_device_get_applied_connection (device);
	g_assert (connection);
	s_bond = nm_connection_get_setting_bond (connection);
	g_assert (s_bond);

	mode_str = nm_setting_bond_get_option_by_name (s_bond, NM_SETTING_BOND_OPTION_MODE);
	if (!mode_str)
		mode_str = "balance-rr";

	mode = _nm_setting_bond_mode_from_string (mode_str);
	if (mode == NM_BOND_MODE_UNKNOWN) {
		_LOGW (LOGD_BOND, "unknown bond mode '%s'", mode_str);
		return NM_ACT_STAGE_RETURN_FAILURE;
	}

	/* Set mode first, as some other options (e.g. arp_interval) are valid
	 * only for certain modes.
	 */

	set_bond_attr (device, mode, NM_SETTING_BOND_OPTION_MODE, mode_str);

	value = nm_setting_bond_get_option_by_name (s_bond, NM_SETTING_BOND_OPTION_MIIMON);
	if (value && atoi (value)) {
		/* clear arp interval */
		set_bond_attr (device, mode, NM_SETTING_BOND_OPTION_ARP_INTERVAL, "0");
		set_arp_interval = FALSE;

		set_bond_attr (device, mode, NM_SETTING_BOND_OPTION_MIIMON, value);
		set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_UPDELAY);
		set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_DOWNDELAY);
	} else if (!value) {
		/* If not given, and arp_interval is not given or disabled, default to 100 */
		value = nm_setting_bond_get_option_by_name (s_bond, NM_SETTING_BOND_OPTION_ARP_INTERVAL);
		if (_nm_utils_ascii_str_to_int64 (value, 10, 0, G_MAXUINT32, 0) == 0)
			set_bond_attr (device, mode, NM_SETTING_BOND_OPTION_MIIMON, "100");
	}

	if (set_arp_interval) {
		set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_ARP_INTERVAL);
		/* Just let miimon get cleared automatically; even setting miimon to
		 * 0 (disabled) clears arp_interval.
		 */
	}

	/* ARP validate: value > 0 only valid in active-backup mode */
	value = nm_setting_bond_get_option_by_name (s_bond, NM_SETTING_BOND_OPTION_ARP_VALIDATE);
	if (   value
	    && !nm_streq (value, "0")
	    && !nm_streq (value, "none")
	    && mode == NM_BOND_MODE_ACTIVEBACKUP)
		set_bond_attr (device, mode, NM_SETTING_BOND_OPTION_ARP_VALIDATE, value);
	else
		set_bond_attr (device, mode, NM_SETTING_BOND_OPTION_ARP_VALIDATE, "0");

	/* Primary */
	value = nm_setting_bond_get_option_by_name (s_bond, NM_SETTING_BOND_OPTION_PRIMARY);
	set_bond_attr (device, mode, NM_SETTING_BOND_OPTION_PRIMARY, value ? value : "");

	/* ARP targets: clear and initialize the list */
	contents = nm_platform_sysctl_master_get_option (NM_PLATFORM_GET, ifindex,
	                                                 NM_SETTING_BOND_OPTION_ARP_IP_TARGET);
	set_arp_targets (device, mode, contents, " \n", "-");
	value = nm_setting_bond_get_option_by_name (s_bond, NM_SETTING_BOND_OPTION_ARP_IP_TARGET);
	set_arp_targets (device, mode, value, ",", "+");
	g_free (contents);

	/* AD actor system: don't set if empty */
	value = nm_setting_bond_get_option_by_name (s_bond, NM_SETTING_BOND_OPTION_AD_ACTOR_SYSTEM);
	if (value)
		set_bond_attr (device, mode, NM_SETTING_BOND_OPTION_AD_ACTOR_SYSTEM, value);

	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_ACTIVE_SLAVE);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_AD_ACTOR_SYS_PRIO);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_AD_SELECT);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_AD_USER_PORT_KEY);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_ALL_SLAVES_ACTIVE);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_ARP_ALL_TARGETS);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_FAIL_OVER_MAC);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_LACP_RATE);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_LP_INTERVAL);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_NUM_GRAT_ARP);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_NUM_UNSOL_NA);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_MIN_LINKS);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_PACKETS_PER_SLAVE);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_PRIMARY_RESELECT);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_RESEND_IGMP);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_TLB_DYNAMIC_LB);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_USE_CARRIER);
	set_simple_option (device, mode, s_bond, NM_SETTING_BOND_OPTION_XMIT_HASH_POLICY);

	return NM_ACT_STAGE_RETURN_SUCCESS;
}

static NMActStageReturn
act_stage1_prepare (NMDevice *dev, NMDeviceStateReason *reason)
{
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_SUCCESS;
	gboolean no_firmware = FALSE;

	g_return_val_if_fail (reason != NULL, NM_ACT_STAGE_RETURN_FAILURE);

	ret = NM_DEVICE_CLASS (nm_device_bond_parent_class)->act_stage1_prepare (dev, reason);
	if (ret != NM_ACT_STAGE_RETURN_SUCCESS)
		return ret;

	/* Interface must be down to set bond options */
	nm_device_take_down (dev, TRUE);
	ret = apply_bonding_config (dev);
	nm_device_bring_up (dev, TRUE, &no_firmware);

	return ret;
}

static void
ip4_config_pre_commit (NMDevice *self, NMIP4Config *config)
{
	NMConnection *connection;
	NMSettingWired *s_wired;
	guint32 mtu;

	connection = nm_device_get_applied_connection (self);
	g_assert (connection);
	s_wired = nm_connection_get_setting_wired (connection);

	if (s_wired) {
		/* MTU override */
		mtu = nm_setting_wired_get_mtu (s_wired);
		if (mtu)
			nm_ip4_config_set_mtu (config, mtu, NM_IP_CONFIG_SOURCE_USER);
	}
}

static gboolean
enslave_slave (NMDevice *device,
               NMDevice *slave,
               NMConnection *connection,
               gboolean configure)
{
	NMDeviceBond *self = NM_DEVICE_BOND (device);
	gboolean success = TRUE, no_firmware = FALSE;
	const char *slave_iface = nm_device_get_ip_iface (slave);

	nm_device_master_check_slave_physical_port (device, slave, LOGD_BOND);

	if (configure) {
		nm_device_take_down (slave, TRUE);
		success = nm_platform_link_enslave (NM_PLATFORM_GET,
		                                    nm_device_get_ip_ifindex (device),
		                                    nm_device_get_ip_ifindex (slave));
		nm_device_bring_up (slave, TRUE, &no_firmware);

		if (!success)
			return FALSE;

		_LOGI (LOGD_BOND, "enslaved bond slave %s", slave_iface);
	} else
		_LOGI (LOGD_BOND, "bond slave %s was enslaved", slave_iface);

	return TRUE;
}

static void
release_slave (NMDevice *device,
               NMDevice *slave,
               gboolean configure)
{
	NMDeviceBond *self = NM_DEVICE_BOND (device);
	gboolean success, no_firmware = FALSE;

	if (configure) {
		success = nm_platform_link_release (NM_PLATFORM_GET,
		                                    nm_device_get_ip_ifindex (device),
		                                    nm_device_get_ip_ifindex (slave));

		if (success) {
			_LOGI (LOGD_BOND, "released bond slave %s",
			       nm_device_get_ip_iface (slave));
		} else {
			_LOGW (LOGD_BOND, "failed to release bond slave %s",
			       nm_device_get_ip_iface (slave));
		}

		/* Kernel bonding code "closes" the slave when releasing it, (which clears
		 * IFF_UP), so we must bring it back up here to ensure carrier changes and
		 * other state is noticed by the now-released slave.
		 */
		if (!nm_device_bring_up (slave, TRUE, &no_firmware))
			_LOGW (LOGD_BOND, "released bond slave could not be brought up.");
	} else {
		_LOGI (LOGD_BOND, "bond slave %s was released",
		       nm_device_get_ip_iface (slave));
	}
}

static gboolean
create_and_realize (NMDevice *device,
                    NMConnection *connection,
                    NMDevice *parent,
                    const NMPlatformLink **out_plink,
                    GError **error)
{
	const char *iface = nm_device_get_iface (device);
	NMPlatformError plerr;

	g_assert (iface);

	plerr = nm_platform_link_bond_add (NM_PLATFORM_GET, iface, out_plink);
	if (plerr != NM_PLATFORM_ERROR_SUCCESS) {
		g_set_error (error, NM_DEVICE_ERROR, NM_DEVICE_ERROR_CREATION_FAILED,
		             "Failed to create bond interface '%s' for '%s': %s",
		             iface,
		             nm_connection_get_id (connection),
		             nm_platform_error_to_string (plerr));
		return FALSE;
	}
	return TRUE;
}

/******************************************************************/

static void
nm_device_bond_init (NMDeviceBond * self)
{
}

static void
nm_device_bond_class_init (NMDeviceBondClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMDeviceClass *parent_class = NM_DEVICE_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMDeviceBondPrivate));

	NM_DEVICE_CLASS_DECLARE_TYPES (klass, NM_SETTING_BOND_SETTING_NAME, NM_LINK_TYPE_BOND)

	parent_class->get_generic_capabilities = get_generic_capabilities;
	parent_class->is_available = is_available;
	parent_class->check_connection_compatible = check_connection_compatible;
	parent_class->check_connection_available = check_connection_available;
	parent_class->complete_connection = complete_connection;

	parent_class->update_connection = update_connection;
	parent_class->master_update_slave_connection = master_update_slave_connection;

	parent_class->create_and_realize = create_and_realize;
	parent_class->act_stage1_prepare = act_stage1_prepare;
	parent_class->ip4_config_pre_commit = ip4_config_pre_commit;
	parent_class->enslave_slave = enslave_slave;
	parent_class->release_slave = release_slave;

	nm_exported_object_class_add_interface (NM_EXPORTED_OBJECT_CLASS (klass),
	                                        NMDBUS_TYPE_DEVICE_BOND_SKELETON,
	                                        NULL);
}

/*************************************************************/

#define NM_TYPE_BOND_FACTORY (nm_bond_factory_get_type ())
#define NM_BOND_FACTORY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_BOND_FACTORY, NMBondFactory))

static NMDevice *
create_device (NMDeviceFactory *factory,
               const char *iface,
               const NMPlatformLink *plink,
               NMConnection *connection,
               gboolean *out_ignore)
{
	return (NMDevice *) g_object_new (NM_TYPE_DEVICE_BOND,
	                                  NM_DEVICE_IFACE, iface,
	                                  NM_DEVICE_DRIVER, "bonding",
	                                  NM_DEVICE_TYPE_DESC, "Bond",
	                                  NM_DEVICE_DEVICE_TYPE, NM_DEVICE_TYPE_BOND,
	                                  NM_DEVICE_LINK_TYPE, NM_LINK_TYPE_BOND,
	                                  NM_DEVICE_IS_MASTER, TRUE,
	                                  NULL);
}

NM_DEVICE_FACTORY_DEFINE_INTERNAL (BOND, Bond, bond,
	NM_DEVICE_FACTORY_DECLARE_LINK_TYPES    (NM_LINK_TYPE_BOND)
	NM_DEVICE_FACTORY_DECLARE_SETTING_TYPES (NM_SETTING_BOND_SETTING_NAME),
	factory_iface->create_device = create_device;
	)

