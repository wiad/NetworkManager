/* NetworkManager -- Network link manager
 *
 * Dan Williams <dcbw@redhat.com>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * (C) Copyright 2004 Red Hat, Inc.
 */

#ifndef NETWORK_MANAGER_DHCP_H
#define NETWORK_MANAGER_DHCP_H

#include "../dhcpcd/dhcpcd.h"

void		nm_device_dhcp_cease		(NMDevice *dev);
gboolean	nm_device_dhcp_setup_timeouts	(NMDevice *dev);
void		nm_device_dhcp_remove_timeouts(NMDevice *dev);
gboolean	nm_device_dhcp_renew		(gpointer user_data);
gboolean	nm_device_dhcp_rebind		(gpointer user_data);


NMIP4Config *	nm_device_new_ip4_autoip_config	(NMDevice *dev);
NMIP4Config *	nm_device_new_ip4_dhcp_config		(NMDevice *dev);

#endif
