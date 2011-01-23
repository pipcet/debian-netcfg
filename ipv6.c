/*
 * IPv6-specific functions for netcfg.
 *
 * Licensed under the terms of the GNU General Public License
 */

#include "netcfg.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <debian-installer.h>

/* Examine the configuration of the given interface, and attempt to determine
 * if it has been assigned an interface via SLAAC.
 *
 * Returns 1 if we think we've got some SLAAC, and 0 otherwise.  Also set
 * interface->slaac to the appropriate boolean value.
 */
int nc_v6_get_slaac(struct netcfg_interface *interface)
{
	FILE *cmdfd;
	char l[256];
	char cmd[512];

	di_debug("Attempting to detect SLAAC for %s", interface->name);
	/* Start with the right default */
	interface->slaac = 0;

#if defined(__FreeBSD_kernel__)
	snprintf(cmd, 512, "ifconfig %s", interface->name);
#else
	snprintf(cmd, 512, "ip addr show %s", interface->name);
#endif
	di_debug("Running %s to look for SLAAC", cmd);
	
	if ((cmdfd = popen(cmd, "r")) != NULL) {
		while (fgets(l, 256, cmdfd) != NULL) {
			di_debug("ip line: %s", l);
			/* Aah, string manipulation in C.  What fun. */
#if defined(__FreeBSD_kernel__)
			if (strncmp("\tinet6 ", l, 7)) {
				continue;
			}
			if (!strstr(l, " autoconf")) {
				continue;
			}
#else
			if (strncmp("    inet6 ", l, 10)) {
				continue;
			}
			if (!strstr(l, " scope global")) {
				continue;
			}
			if (!strstr(l, " dynamic")) {
				/* Hmm, a global address that isn't SLAAC?
				 * Strange at this point, but not what we're
				 * after.
				 */
				continue;
			}
#endif

			/* So, we've found a dynamically-assigned global
			 * inet6 address.  Sounds like SLAAC to me.
			 */
			di_info("Detected SLAAC is in use on %s", interface->name);
			interface->slaac = 1;
			interface->address_family = AF_INET6;
			break;
		}
	}

	pclose(cmdfd);
	return interface->slaac;
}
