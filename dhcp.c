/*
 * DHCP module for netcfg/netcfg-dhcp.
 *
 * Licensed under the terms of the GNU General Public License
 */

#include "netcfg.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <debian-installer.h>
#include <stdio.h>
#include <assert.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <time.h>
#include <netdb.h>

#define DHCP_OPTION_LEN 1236 /* pump 0.8.24 defines a max option size of 57,
                                dhcp 2.0pl5 uses 1222, dhcp3 3.0.6 uses 1236 */

const char* dhclient_request_options_dhclient[] = { "subnet-mask",
                                                    "broadcast-address",
                                                    "time-offset",
                                                    "routers",
                                                    "domain-name",
                                                    "domain-name-servers",
                                                    "host-name",
                                                    "ntp-servers", /* extra */
                                                    NULL };

const char* dhclient_request_options_udhcpc[] = { "subnet",
                                                  "broadcast",
                                                  "router",
                                                  "domain",
                                                  "hostname",
                                                  "dns",
                                                  "ntpsrv", /* extra */
                                                  NULL };

static int dhcp_exit_status = 1;
static pid_t dhcp_pid = -1;

/* Returns 1 if no default route is available */
static short no_default_route (void)
{
#if defined(__FreeBSD_kernel__)
    int status4, status6;

    status4 = system("exec /lib/freebsd/route show default >/dev/null 2>&1");
    status6 = system("exec /lib/freebsd/route -6 show default >/dev/null 2>&1");

    return (WEXITSTATUS(status4) != 0) && (WEXITSTATUS(status6) != 0);
#elif defined(__GNU__)
    FILE* pfinet = NULL;
    char buf[1024] = { 0 };

    pfinet = popen("fsysopts /servers/socket/2", "r");
    if (!pfinet)
        return 1;

    if (fgets (buf, 1024, pfinet) == NULL) {
        pclose (pfinet);
        return 1;
    }
    pclose (pfinet);

    return !strstr (buf, "--gateway=");
#else
    FILE* iproute = NULL;
    char buf[256] = { 0 };

    /* IPv4 default route? */
    if ((iproute = popen("ip route", "r")) != NULL) {
        while (fgets (buf, 256, iproute) != NULL) {
            if (strncmp(buf, "default via ", 12) == 0) {
                pclose(iproute);
                return 0;
            }
        }
        pclose(iproute);
    }

    /* IPv6 default route? */
    if ((iproute = popen("ip -6 route", "r")) != NULL) {
        while (fgets (buf, 256, iproute) != NULL) {
            if (strncmp(buf, "default via ", 12) == 0) {
                pclose(iproute);
                return 0;
            }
        }
        pclose(iproute);
    }

    return 1;
#endif
}

/*
 * Signal handler for DHCP client child
 *
 * When the child exits (either because it failed to obtain a
 * lease or because it succeeded and daemonized itself), this
 * gets the child's exit status and sets dhcp_pid to -1
 */
static void cleanup_dhcp_client(void)
{
    if (dhcp_pid <= 0)
        /* Already cleaned up */
        return;

    di_debug("Waiting for dhcp_pid %i", dhcp_pid);
    waitpid(dhcp_pid, &dhcp_exit_status, WNOHANG);
    if (WIFEXITED(dhcp_exit_status)) {
        di_debug("DHCP client exited");
        dhcp_pid = -1;
    }
}

/* Run through the available client process handlers we're running, and tell
 * them to cleanup if required.
 */
static void sigchld_handler(int sig __attribute__ ((unused)))
{
    di_debug("SIGCHLD received; handling");
    cleanup_dhcp_client();
    cleanup_rdnssd();
    di_debug("SIGCHLD handler finished");
}

/*
 * This function will start whichever DHCP client is available
 * using the provided DHCP hostname, if supplied
 *
 * The client's PID is stored in dhcp_pid.
 */
int start_dhcp_client (struct debconfclient *client, char* dhostname, const char *if_name)
{
    FILE *dc = NULL;
    const char **ptr;
    char **arguments;
    int options_count;
    enum { DHCLIENT, PUMP, UDHCPC } dhcp_client;
    int dhcp_seconds;
    char dhcp_seconds_str[16];

    if (access("/sbin/dhclient", F_OK) == 0)
            dhcp_client = DHCLIENT;
    else if (access("/sbin/pump", F_OK) == 0)
        dhcp_client = PUMP;
    else if (access("/sbin/udhcpc", F_OK) == 0)
        dhcp_client = UDHCPC;
    else {
        debconf_input(client, "critical", "netcfg/no_dhcp_client");
        debconf_go(client);
        exit(1);
    }

    debconf_get(client, "netcfg/dhcp_timeout");
    dhcp_seconds = atoi(client->value);
    snprintf(dhcp_seconds_str, sizeof dhcp_seconds_str, "%d", dhcp_seconds-1);

    if ((dhcp_pid = fork()) == 0) { /* child */
        /* disassociate from debconf */
        fclose(client->out);

        /* get dhcp lease */
        switch (dhcp_client) {
        case PUMP:
            if (dhostname)
                execlp("pump", "pump", "-i", if_name, "-h", dhostname, NULL);
            else
                execlp("pump", "pump", "-i", if_name, NULL);

            break;

        case DHCLIENT:
            /* First, set up dhclient.conf */
            if ((dc = file_open(DHCLIENT_CONF, "w"))) {
                fprintf(dc, "send vendor-class-identifier \"d-i\";\n" );
                fprintf(dc, "request ");

                for (ptr = dhclient_request_options_dhclient; *ptr; ptr++) {
                    fprintf(dc, "%s", *ptr);

                    /* look ahead to see if it is the last entry */
                    if (*(ptr + 1))
                        fprintf(dc, ", ");
                    else
                        fprintf(dc, ";\n");
                }

                if (dhostname) {
                    fprintf(dc, "send host-name \"%s\";\n", dhostname);
                }
                fprintf(dc, "timeout %d;\n", dhcp_seconds);
                fprintf(dc, "initial-interval 1;\n");
                fclose(dc);
            }

            execlp("dhclient", "dhclient", "-1", if_name, "-cf", DHCLIENT_CONF, NULL);
            break;

        case UDHCPC:
            /* figure how many options we have */
            options_count = 0;
            for (ptr = dhclient_request_options_udhcpc; *ptr; ptr++)
                options_count++;

            arguments = malloc((options_count * 2  /* -O <option> repeatedly */
                                + 9    /* Other arguments (listed below) */
                                + 2    /* dhostname (maybe) */
                                + 1    /* NULL */
                               ) * sizeof(char **));

            /* set the command options */
            options_count = 0;
            arguments[options_count++] = "udhcpc";
            arguments[options_count++] = "-i";
            arguments[options_count++] = (char *)if_name;
            arguments[options_count++] = "-V";
            arguments[options_count++] = "d-i";
            arguments[options_count++] = "-T";
            arguments[options_count++] = "1";
            arguments[options_count++] = "-t";
            arguments[options_count++] = dhcp_seconds_str;
            for (ptr = dhclient_request_options_udhcpc; *ptr; ptr++) {
                arguments[options_count++] = "-O";
                arguments[options_count++] = (char *)*ptr;
            }

            if (dhostname) {
                arguments[options_count++] = "-H";
                arguments[options_count++] = dhostname;
            }

            arguments[options_count] = NULL;

            execvp("udhcpc", arguments);
            free(arguments);
            break;
        }
        if (errno != 0)
            di_error("Could not exec dhcp client: %s", strerror(errno));

        return 1; /* should NEVER EVER get here */
    }
    else if (dhcp_pid == -1) {
        di_warning("DHCP fork failed; this is unlikely to end well");
        return 1;
    } else {
        /* dhcp_pid contains the child's PID */
        di_warning("Started DHCP client; PID is %i", dhcp_pid);
        signal(SIGCHLD, &sigchld_handler);
        return 0;
    }
}

static int kill_dhcp_client(void)
{
    if (system("kill-all-dhcp")) {
        /* We can't do much about errors anyway, so ignore them. */
    }
    return 0;
}

/*
 * Poll the started DHCP client for netcfg/dhcp_timeout seconds (def. 15)
 * and return 0 if a lease is known to have been acquired,
 * 1 otherwise.
 *
 * The client should be run such that it exits once a lease is acquired
 * (although its child continues to run as a daemon)
 *
 * This function will NOT kill the child if time runs out.  This allows
 * the user to choose to wait longer for the lease to be acquired.
 */
int poll_dhcp_client (struct debconfclient *client)
{
    int seconds_slept = 0;
    int ret = 1;
    int dhcp_seconds;

    debconf_get(client, "netcfg/dhcp_timeout");

    dhcp_seconds = atoi(client->value);

    /* show progress bar */
    debconf_capb(client, "backup progresscancel");
    debconf_progress_start(client, 0, dhcp_seconds, "netcfg/dhcp_progress");
    if (debconf_progress_info(client, "netcfg/dhcp_progress_note") ==
            CMD_PROGRESSCANCELLED) {
        kill_dhcp_client();
        goto stop;
    }

    /* wait between 2 and dhcp_seconds seconds for a DHCP lease */
    while ( ((dhcp_pid > 0) || (seconds_slept < 2))
            && (seconds_slept < dhcp_seconds) ) {
        sleep(1);
        seconds_slept++; /* Not exact but close enough */
        if (debconf_progress_step(client, 1) == CMD_PROGRESSCANCELLED)
            goto stop;
    }
    /* Either the client exited or time ran out */

    /* got a lease? display a success message */
    if (!(dhcp_pid > 0) && (dhcp_exit_status == 0)) {
        ret = 0;

        debconf_capb(client, "backup"); /* stop displaying cancel button */
        if (debconf_progress_set(client, dhcp_seconds) ==
                CMD_PROGRESSCANCELLED)
            goto stop;
        if (debconf_progress_info(client, "netcfg/dhcp_success_note") ==
                CMD_PROGRESSCANCELLED)
            goto stop;
        sleep(2);
    }

 stop:
    /* stop progress bar */
    debconf_progress_stop(client);
    debconf_capb(client, "backup");

    return ret;
}


#define REPLY_RETRY_AUTOCONFIG       0
#define REPLY_RETRY_WITH_HOSTNAME    1
#define REPLY_CONFIGURE_MANUALLY     2
#define REPLY_DONT_CONFIGURE         3
#define REPLY_RECONFIGURE_WIFI       4
#define REPLY_LOOP_BACK              5
#define REPLY_CHECK_DHCP             6
#define REPLY_ASK_OPTIONS            7

int ask_dhcp_options (struct debconfclient *client, const char *if_name)
{
    if (is_wireless_iface(if_name)) {
        debconf_metaget(client, "netcfg/internal-wifireconf", "description");
        debconf_subst(client, "netcfg/dhcp_options", "wifireconf", client->value);
    }
    else /* blank from last time */
        debconf_subst(client, "netcfg/dhcp_options", "wifireconf", "");

    /* critical, we don't want to enter a loop */
    debconf_input(client, "critical", "netcfg/dhcp_options");

    if (debconf_go(client) == CMD_GOBACK)
        return GO_BACK;

    debconf_get(client, "netcfg/dhcp_options");

    /* strcmp sucks */
    if (client->value[0] == 'R') {      /* _R_etry ... or _R_econfigure ... */
        size_t len = strlen(client->value);
        if (client->value[len - 1] == 'e') /* ... with DHCP hostnam_e_ */
            return REPLY_RETRY_WITH_HOSTNAME;
        else if (client->value[len - 1] == 'k') /* ... wireless networ_k_ */
            return REPLY_RECONFIGURE_WIFI;
        else
            return REPLY_RETRY_AUTOCONFIG;
    }
    else if (client->value[0] == 'C') /* _C_onfigure ... */
        return REPLY_CONFIGURE_MANUALLY;
    else if (empty_str(client->value))
        return REPLY_LOOP_BACK;
    else
        return REPLY_DONT_CONFIGURE;
}

int ask_wifi_configuration (struct debconfclient *client, struct netcfg_interface *interface)
{
    enum { ABORT, ESSID, SECURITY_TYPE, WEP, WPA, START, DONE } wifistate = ESSID;

    if (interface->wpa_supplicant_status != WPA_UNAVAIL)
        kill_wpa_supplicant();

    for (;;) {
        switch (wifistate) {
        case ESSID:
            if (interface->wpa_supplicant_status == WPA_UNAVAIL)
                wifistate = (netcfg_wireless_set_essid(client, interface) == GO_BACK) ?
                    ABORT : WEP;
            else
                wifistate = (netcfg_wireless_set_essid(client, interface) == GO_BACK) ?
                    ABORT : SECURITY_TYPE;
            break;

        case SECURITY_TYPE:
            {
                int ret;
                ret = wireless_security_type(client, interface->name);
                if (ret == GO_BACK)
                    wifistate = ESSID;
                else if (ret == REPLY_WPA)
                    wifistate = WPA;
                else
                    wifistate = WEP;
                break;
            }

        case WEP:
            if (interface->wpa_supplicant_status == WPA_UNAVAIL)
                wifistate = (netcfg_wireless_set_wep(client, interface) == GO_BACK) ?
                    ESSID : DONE;
            else
                wifistate = (netcfg_wireless_set_wep(client, interface) == GO_BACK) ?
                    SECURITY_TYPE : DONE;
            break;

        case WPA:
            wifistate = (netcfg_set_passphrase(client, interface) == GO_BACK) ?
                SECURITY_TYPE : START;
            break;

        case START:
            wifistate = (wpa_supplicant_start(client, interface) == GO_BACK) ?
                ESSID : DONE;
            break;

        case ABORT:
            return REPLY_ASK_OPTIONS;
            break;

        case DONE:
            return REPLY_CHECK_DHCP;
            break;
        }
    }
}


int netcfg_activate_dhcp (struct debconfclient *client, struct netcfg_interface *interface)
{
    enum { START, POLL, CHECK_SLAAC, DEFAULT_GATEWAY, NAMESERVERS,
           ASK_OPTIONS, DHCP_HOSTNAME, HOSTNAME, DOMAIN, HOSTNAME_SANS_NETWORK
           } state = START;
    char nameserver_array[4][NETCFG_ADDRSTRLEN];
    char *if_name = interface->name;

    kill_dhcp_client();
    loop_setup();
    
    interface->dhcp = 1;

    for (;;) {
        di_debug("State is now %i", state);
        switch (state) {
        case START:
            if (start_dhcp_client(client, interface->dhcp_hostname, if_name))
                netcfg_die(client); /* change later */
            if (!start_rdnssd(client))
                netcfg_die(client); /* change later */
                
            state = POLL;
            break;

        case POLL:
            if (poll_dhcp_client(client)) {
                state = CHECK_SLAAC;
            } else {
                /*
                 * Set defaults for domain name and hostname
                 */
                FILE *d = NULL;

                have_domain = 0;

                /*
                 * Default to the domain name returned via DHCP, if any
                 */
                if ((d = fopen(DOMAIN_FILE, "r")) != NULL) {
                    char domain[_UTSNAME_LENGTH + 1] = { 0 };
                    if (fgets(domain, _UTSNAME_LENGTH, d) == NULL) {
                        /* ignore errors; we check for empty strings later */
                    }
                    fclose(d);
                    unlink(DOMAIN_FILE);

                    if (!empty_str(domain) && valid_domain(domain)) {
                        debconf_set(client, "netcfg/get_domain", domain);
                        have_domain = 1;
                    }
                }

                /*
                 * Record any ntp server information from DHCP for later
                 * verification and use by clock-setup
                 */
                if ((d = fopen(NTP_SERVER_FILE, "r")) != NULL) {
                    char ntpservers[DHCP_OPTION_LEN + 1] = { 0 };
                    if (fgets(ntpservers, DHCP_OPTION_LEN, d) == NULL) {
                        /* ignore errors; we check for empty strings later */
                    }
                    fclose(d);
                    unlink(NTP_SERVER_FILE);

                    if (!empty_str(ntpservers)) {
                        debconf_set(client, "netcfg/dhcp_ntp_servers",
                                    ntpservers);
                    }
                }


                state = DEFAULT_GATEWAY;
            }
            break;
        case CHECK_SLAAC:
            if (nc_v6_get_slaac(interface)) {
                /* We won't be needing this any more */
                kill_dhcp_client();
                stop_rdnssd();
                read_resolv_conf_nameservers("/tmp/rdnssd_resolv", nameserver_array, ARRAY_SIZE(nameserver_array));
                if (resolv_conf_entries(nameserver_array, ARRAY_SIZE(nameserver_array)) > 0) {
                    /* Our RAs have DNS entries!  Better install rdnssd to
                     * keep track of them
                     */
                    di_exec_shell_log("apt-install rdnssd");
                }
                interface->dhcp = 0;
                state = DEFAULT_GATEWAY;
            } else {
                /* autoconfiguration has most definitely failed */
                debconf_capb(client, "");
                debconf_input(client, "critical", "netcfg/dhcp_failed");
                debconf_go(client);
                debconf_capb(client, "backup");
                state = ASK_OPTIONS;
	    }
	    break;
	case DEFAULT_GATEWAY:
            if (no_default_route()) {
                debconf_input(client, "critical", "netcfg/no_default_route");
                debconf_go(client);
                debconf_get(client, "netcfg/no_default_route");

                if (!strcmp(client->value, "false")) {
                    state = ASK_OPTIONS;
                    break;
                }
            }
            state = NAMESERVERS;
            break;
        case NAMESERVERS:
            /* Make sure we have NS going if the DHCP server didn't serve it up */
            read_resolv_conf_nameservers(RESOLV_FILE, nameserver_array, ARRAY_SIZE(nameserver_array));
            if (resolv_conf_entries(nameserver_array, ARRAY_SIZE(nameserver_array)) == 0) {
                char *nameservers = NULL;

                if (netcfg_get_nameservers (client, &nameservers, NULL) == GO_BACK) {
                    state = ASK_OPTIONS;
                    break;
                }

                netcfg_nameservers_to_array (nameservers, nameserver_array, ARRAY_SIZE(nameserver_array));
            }

            /* We don't have a domain name yet, but we need to write out the
             * nameservers now so we can do rDNS lookups later to possibly
             * find out the domain.
             */
            netcfg_write_resolv(NULL, nameserver_array, ARRAY_SIZE(nameserver_array));
            state = HOSTNAME;
            break;
        case ASK_OPTIONS:
            /* DHCP client may still be running */
            switch (ask_dhcp_options (client, if_name)) {
            case GO_BACK:
                kill_dhcp_client();
                stop_rdnssd();
                interface->dhcp = 0;
                return RETURN_TO_MAIN;
            case REPLY_RETRY_WITH_HOSTNAME:
                state = DHCP_HOSTNAME;
                break;
            case REPLY_CONFIGURE_MANUALLY:
                kill_dhcp_client();
                stop_rdnssd();
                interface->dhcp = 0;
                return CONFIGURE_MANUALLY;
            case REPLY_DONT_CONFIGURE:
                kill_dhcp_client();
                stop_rdnssd();
                state = HOSTNAME_SANS_NETWORK;
                break;
            case REPLY_RETRY_AUTOCONFIG:
                if (dhcp_pid > 0)
                    state = POLL;
                else {
                    kill_dhcp_client();
                    stop_rdnssd();
                    state = START;
                }
                break;
            case REPLY_RECONFIGURE_WIFI:
                if (ask_wifi_configuration(client, interface) == REPLY_CHECK_DHCP) {
                    kill_dhcp_client();
                    stop_rdnssd();
                    state = START;
                }
                else
                    state = ASK_OPTIONS;
                break;
            }
            break;

        case DHCP_HOSTNAME:
            /* DHCP client may still be running */
            if (netcfg_get_hostname(client, "netcfg/dhcp_hostname", interface->dhcp_hostname, 0))
                state = ASK_OPTIONS;
            else {
                kill_dhcp_client();
                stop_rdnssd();
                state = START;
            }
            break;

        case HOSTNAME:
            {
                char buf[MAXHOSTNAMELEN + 1] = { 0 };
                /*
                 * Default to the hostname returned via DHCP, if any,
                 * otherwise to the requested DHCP hostname
                 * otherwise to the hostname found in DNS for the IP address
                 * of the interface
                 */
                if (gethostname(buf, sizeof(buf)) == 0
                    && !empty_str(buf)
                    && strcmp(buf, "(none)")
                    ) {
                    di_info("DHCP hostname: \"%s\"", buf);
                }
                else if (!empty_str(interface->dhcp_hostname)) {
                    di_debug("Defaulting hostname to provided DHCP hostname");
                    debconf_set(client, "netcfg/get_hostname", interface->dhcp_hostname);
                } else {
                    di_debug("Using DNS to try and obtain default hostname");
                    get_hostname_from_dns(interface, buf, sizeof(buf));
                }

                preseed_hostname_from_fqdn(client, buf);

                if (netcfg_get_hostname (client, "netcfg/get_hostname", hostname, 1)) {
                    /*
                     * Going back to POLL wouldn't make much sense.
                     * However, it does make sense to go to the retry
                     * screen where the user can elect to retry DHCP with
                     * a requested DHCP hostname, etc.
                     */
                    state = ASK_OPTIONS;
                }
                else
                    state = DOMAIN;
            }
            break;

        case DOMAIN:
            if (!have_domain && netcfg_get_domain (client, &domain))
                state = HOSTNAME;
            else {
                di_debug("Network config complete");
                netcfg_write_common("", hostname, domain);
                netcfg_write_loopback();
                netcfg_write_interface(interface);
                netcfg_write_resolv(domain, nameserver_array, ARRAY_SIZE(nameserver_array));
                kill_dhcp_client();
                stop_rdnssd();

                return 0;
            }
            break;

        case HOSTNAME_SANS_NETWORK:
            if (netcfg_get_hostname (client, "netcfg/get_hostname", hostname, 0))
                state = ASK_OPTIONS;
            else {
                netcfg_write_common("", hostname, NULL);
                interface->dhcp = 0;
                return 0;
            }
            break;
        }
    }
}

/* Count the number of actual entries in the nameservers array */
int resolv_conf_entries (char nameservers[][NETCFG_ADDRSTRLEN], const unsigned int ns_size)
{
    unsigned int count = 0, i;
    
    for (i = 0; i < ns_size; i++) {
        if (!empty_str(nameservers[i])) count++;
    }

    return count;
}

/* Read the nameserver entries out of resolv.conf and stick them into
 * array, so we can write out a newer, shinier resolv.conf
 */
int read_resolv_conf_nameservers(char *resolv_conf_file, char array[][NETCFG_ADDRSTRLEN], unsigned int array_size)
{
    FILE *f;
    unsigned int i = 0;
    
    di_debug("Reading nameservers from %s", resolv_conf_file);
    if ((f = fopen(resolv_conf_file, "r")) != NULL) {
        char buf[256];

        while (fgets(buf, 256, f) != NULL) {
            char *ptr;

            if (strncmp(buf, "nameserver ", strlen("nameserver ")) == 0) {
                /* Chop off trailing \n */
                if (buf[strlen(buf)-1] == '\n')
                    buf[strlen(buf)-1] = '\0';

                ptr = buf + strlen("nameserver ");
                strncpy(array[i], ptr, NETCFG_ADDRSTRLEN);
                di_debug("Read nameserver %s", array[i]);
                i++;
                if (i == array_size) {
                    /* We can only hold so many nameservers, and we've reached
                     * our limit.  Sorry.
                     */
                    break;
                }
            }
        }

        fclose(f);
        /* Null out any remaining elements in array */
        for (; i < array_size; i++) array[i][0] = '\0';

        return 1;
    }
    else
        return 0;
}
