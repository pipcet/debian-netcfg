#ifndef _NETCFG_H_
#define _NETCFG_H_

#define INTERFACES_FILE "/etc/network/interfaces"
#define HOSTS_FILE      "/etc/hosts"
#define HOSTNAME_FILE   "/etc/hostname"
#define NETWORKS_FILE   "/etc/networks"
#define RESOLV_FILE     "/etc/resolv.conf"
#define DHCLIENT_CONF   "/etc/dhclient.conf"
#define DOMAIN_FILE     "/tmp/domain_name"
#define NTP_SERVER_FILE "/tmp/dhcp-ntp-servers"
#define WPASUPP_CTRL    "/var/run/wpa_supplicant"
#define WPAPID          "/var/run/wpa_supplicant.pid"

#define DEVNAMES	"/etc/network/devnames"
#define DEVHOTPLUG	"/etc/network/devhotplug"
#ifdef __linux__
#define STAB		"/var/run/stab"
#endif

#define WPA_MIN         8    /* minimum passphrase length */
#define WPA_MAX         64   /* maximum passphrase length */

#define _GNU_SOURCE

#include <sys/types.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <cdebconf/debconfclient.h>

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif

#define empty_str(s) (s != NULL && *s == '\0')

#define HELPFUL_COMMENT \
"# This file describes the network interfaces available on your system\n" \
"# and how to activate them. For more information, see interfaces(5).\n"

#define IPV6_HOSTS \
"# The following lines are desirable for IPv6 capable hosts\n" \
"::1     ip6-localhost ip6-loopback\n" \
"fe00::0 ip6-localnet\n" \
"ff00::0 ip6-mcastprefix\n" \
"ff02::1 ip6-allnodes\n" \
"ff02::2 ip6-allrouters\n"

/* The number of times to attempt to verify gateway reachability.
 * Each try invokes arping with a one second timeout.
 */
#define NETCFG_GATEWAY_REACHABILITY_TRIES 50

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 63
#endif

#define RETURN_TO_MAIN 10
#define CONFIGURE_MANUALLY 15

typedef enum { NOT_ASKED = 30, GO_BACK, REPLY_WEP, REPLY_WPA } response_t;
typedef enum { DHCP, STATIC, DUNNO } method_t;
typedef enum { ADHOC = 1, MANAGED = 2 } wifimode_t;
extern enum wpa_t { WPA_OK, WPA_QUEUED, WPA_UNAVAIL } wpa_supplicant_status;

extern int wfd, skfd;
extern int input_result;
extern int have_domain;

/* network config */
extern char *interface;
extern char *hostname;
extern char *dhcp_hostname;
extern char *domain;

/* wireless */
extern char *essid, *wepkey, *passphrase;
extern wifimode_t mode;

/* common functions */
extern int check_kill_switch (const char *iface);

extern int is_interface_up (char *inter);

extern void get_name (char *name, char *p);

extern int get_all_ifs (int all, char ***ptr);

extern char *get_ifdsc (struct debconfclient *client, const char *ifp);

extern FILE *file_open (char *path, const char *opentype);

extern void netcfg_die (struct debconfclient *client);

extern int netcfg_get_interface(struct debconfclient *client, char **interface, int *num_interfaces, const char *defif);

extern short valid_hostname (const char *hname);
extern short valid_domain (const char *dname);

extern int netcfg_get_hostname(struct debconfclient *client, char *template, char **hostname, short hdset);

extern int netcfg_get_nameservers (struct debconfclient *client, char **nameservers, char *default_nameservers);

extern int netcfg_get_domain(struct debconfclient *client,  char **domain);

extern int netcfg_get_static(struct debconfclient *client);

extern int netcfg_activate_dhcp(struct debconfclient *client);

extern int resolv_conf_entries (void);

extern int read_resolv_conf_nameservers (char nameservers[][INET_ADDRSTRLEN], unsigned int ns_size);

extern int ask_dhcp_options (struct debconfclient *client);
extern int netcfg_activate_static(struct debconfclient *client,
                                  const char *ipaddress,
                                  const char *gateway,
                                  const char *pointopoint,
                                  const char *netmask);

extern void netcfg_write_loopback (void);
extern void netcfg_write_common (const char *ipaddress, const char *hostname, const char *domain);

void netcfg_nameservers_to_array(char *nameservers, char array[][INET_ADDRSTRLEN], unsigned int array_size);

extern int is_wireless_iface (const char* iface);
extern int netcfg_wireless_set_essid (struct debconfclient *client, char* iface);
extern int netcfg_wireless_set_wep (struct debconfclient *client, char* iface);
extern int wireless_security_type (struct debconfclient *client, char* iface);
extern int netcfg_set_passphrase (struct debconfclient *client, char* iface);
extern int init_wpa_supplicant_support (void);
extern int kill_wpa_supplicant (void);

extern int wpa_supplicant_start (struct debconfclient *client, char *iface, char *ssid, char *passphrase);
extern int iface_is_hotpluggable(const char *iface);
extern short find_in_stab (const char *iface);
extern void deconfigure_network(void);

extern void interface_up (char*);
extern void interface_down (char*);

extern void loop_setup(void);
extern void seed_hostname_from_dns(struct debconfclient *client, const char *ipaddress);

extern int inet_ptom (const char *src, int *dst);
extern const char *inet_mtop (int src, char *dst, socklen_t dst_len);

extern void parse_args (int argc, char** argv);
extern void open_sockets (void);
extern void reap_old_files (void);

extern void netcfg_update_entropy (void);

extern int netcfg_write_resolv (char*, char nameservers[][INET_ADDRSTRLEN], unsigned int nameservers_size);

extern int ethtool_lite (const char *if_name);
extern int netcfg_detect_link(struct debconfclient *client, const char *if_name, const char *gateway);

#endif /* _NETCFG_H_ */
