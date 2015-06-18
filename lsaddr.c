#include <argp.h>
#include <arpa/inet.h>
#include <error.h>
#include <net/if.h>
#include <netinet/in.h>
#include <search.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#if INET_ADDRSTRLEN > INET6_ADDRSTRLEN
#define ADDRSTRLEN #INET_ADDRSTRLEN
#else
#define ADDRSTRLEN INET6_ADDRSTRLEN
#endif

/* Keys for command line options without short options */
#define OPT_INCLUDE_LOOPBACK 1
#define OPT_INCLUDE_LINK_LOCAL 2
#define OPT_LIST_INTERFACES 3

#define PROC_NET_DEV_PATH "/proc/net/dev"
#define PROC_NET_IF_INET6_PATH "/proc/net/if_inet6"

#define PROC_NET_DEV_HEADER_LINES 2

static struct argp_option options[] = {
    {"ipv4", '4', 0, 0, "List IPv4 addresses", 0},
    {"ipv6", '6', 0, 0, "List IPv6 addresses", 0},
    {"include-loopback", OPT_INCLUDE_LOOPBACK, 0, 0,
     "Include addresses for the loopback interface", 0},
    {"include-link-local", OPT_INCLUDE_LINK_LOCAL, 0, 0,
     "Include IPv6 link-local addresses", 0},
    {"list-interfaces", OPT_LIST_INTERFACES, 0, 0, "List interfaces and exit",
     0},
    {0}};

/* Command line options */
struct args {
  bool ip_version_specified;
  bool ipv4;
  bool ipv6;
  bool include_loopback;
  bool include_link_local;
  bool interfaces_specified;
  bool list_interfaces;
  char **interfaces;
  size_t num_interfaces;
};

static error_t parse_opt(int key, char *arg __attribute__((unused)),
                         struct argp_state *state) {
  struct args *args = state->input;

  switch (key) {
    case '4':
      args->ip_version_specified = true;
      args->ipv4 = true;
      break;
    case '6':
      args->ip_version_specified = true;
      args->ipv6 = true;
      break;
    case OPT_INCLUDE_LOOPBACK:
      args->include_loopback = true;
      break;
    case OPT_INCLUDE_LINK_LOCAL:
      args->include_link_local = true;
      break;
    case OPT_LIST_INTERFACES:
      args->list_interfaces = true;
      break;
    case ARGP_KEY_ARGS:
      args->interfaces_specified = true;
      args->interfaces = state->argv + state->next;
      args->num_interfaces = state->argc - state->next;
      break;
    default:
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

static struct argp argp = {options, parse_opt, 0, 0, 0, 0, 0};

int get_interfaces(char ***interfaces __attribute__((unused)),
                   size_t *num_interfaces __attribute__((unused))) {
  FILE *proc_net_dev = fopen(PROC_NET_DEV_PATH, "r");
  if (!proc_net_dev) {
    error(EXIT_FAILURE, errno,
          "error listing interfaces: could not open file %s",
          PROC_NET_DEV_PATH);
  }

  /* Skip header lines */
  char *line = NULL;
  size_t len = 0;
  for (int i = 0; i < PROC_NET_DEV_HEADER_LINES; ++i) {
    if (getline(&line, &len, proc_net_dev) == -1) {
      goto errorparse;
    }
  }

  /* Save location of first interface line for after counting */
  fpos_t first_ifc_line;
  if (fgetpos(proc_net_dev, &first_ifc_line)) {
    goto errorparse;
  }

  /* Count interface lines */
  *num_interfaces = 0;
  for (; getline(&line, &len, proc_net_dev) != -1; ++*num_interfaces)
    ;
  if (ferror(proc_net_dev)) {
    goto errorparse;
  }

  *interfaces = calloc(sizeof(char *), *num_interfaces);

  /* Jump back and parse the lines */
  if (fsetpos(proc_net_dev, &first_ifc_line)) {
    goto errorparse;
  }

  for (size_t i = 0;
       i < *num_interfaces && getline(&line, &len, proc_net_dev) != -1; ++i) {
    sscanf(line, "%ms", *interfaces + i);
    *index((*interfaces)[i], ':') = '\0';
  }
  if (ferror(proc_net_dev)) {
    goto errorparse;
  }

  return 0;

errorparse:
  error(EXIT_FAILURE, errno,
        "error listing interfaces: could not parse file %s", PROC_NET_DEV_PATH);
  return -1;
}

/* Removes bad interface names from the `interfaces` array.
 *
 * Uses `sockfd` to check interfaces exist. Any non-existent or otherwise
 * inaccessible interfaces are removed from `interfaces`, and `num_interfaces`
 * is updated accordingly.
 *
 * Returns 0 on success, otherwise it returns -1, and `errno` is set according
 * to the last failure that ocurred.
 */
int remove_bad_interfaces(int sockfd, char **interfaces,
                          size_t *num_interfaces) {
  // TODO: check args
  struct ifreq req;
  size_t in_ix, out_ix;
  errno = 0;
  for (in_ix = 0, out_ix = 0; in_ix < *num_interfaces; ++in_ix) {
    strncpy(req.ifr_name, interfaces[in_ix], IFNAMSIZ);
    if (ioctl(sockfd, SIOCGIFINDEX, &req)) {
      error(0 /* status */, errno, "could not open interface %s",
            interfaces[in_ix]);
      continue;
    }
    interfaces[out_ix++] = interfaces[in_ix];
  }
  *num_interfaces = out_ix;

  return errno ? -1 : 0;
}

int cmp(const void *left, const void *right) {
  char **left_sp = (char **)left;
  char **right_sp = (char **)right;

  return strcmp(*left_sp, *right_sp);
}

int main(int argc, char **argv) {
  struct args args = {
      .ip_version_specified = false,
      .ipv4 = false,
      .ipv6 = false,
      .include_loopback = false,
      .include_link_local = false,
      .interfaces_specified = false,
      .list_interfaces = false,
      .interfaces = NULL,
      .num_interfaces = 0,
  };

  argp_parse(&argp, argc, argv, 0, 0, &args);

  char **interfaces;
  size_t num_interfaces;
  get_interfaces(&interfaces, &num_interfaces);

  if (args.list_interfaces) {
    for (size_t i = 0; i < num_interfaces; ++i) {
      printf("%s\n", interfaces[i]);
    }
    exit(EXIT_SUCCESS);
  }

  if (access("/proc/net", R_OK)) {
    goto errorout;
  }
  if (access("/proc/net/if_inet6", R_OK)) {
    goto errorout;
  }

  int sockfd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_IP);
  if (sockfd == -1) {
    goto errorout;
  }

  remove_bad_interfaces(sockfd, args.interfaces, &args.num_interfaces);

  struct ifconf stuff = {.ifc_len = 0, .ifc_buf = NULL};

  /* Ask for the correct buffer size */
  if (ioctl(sockfd, SIOCGIFCONF, &stuff)) {
    goto errorout;
  }
  if (!(stuff.ifc_buf = malloc(stuff.ifc_len))) {
    goto errorout;
  }

  /* Now ask for the interfaces */
  if (ioctl(sockfd, SIOCGIFCONF, &stuff)) {
    goto errorout;
  }

  char addr[ADDRSTRLEN];

  for (size_t i = 0, len = stuff.ifc_len / sizeof(struct ifreq); i < len; ++i) {
    char *ifc = stuff.ifc_req[i].ifr_name;
    if (!args.interfaces_specified ||
        lfind(&ifc, args.interfaces, &args.num_interfaces, sizeof(char *),
              cmp)) {
      if (!args.ip_version_specified || args.ipv4) {
        struct sockaddr_in *sockaddr =
            (struct sockaddr_in *)&stuff.ifc_req[i].ifr_addr;
        printf("%s\n",
               inet_ntop(AF_INET, &sockaddr->sin_addr, addr, sizeof(addr)));
      }
    }
  }

  if (!args.ip_version_specified || args.ipv6) {
    FILE *if_inet6 = fopen(PROC_NET_IF_INET6_PATH, "r");
    if (!if_inet6) {
      error(0 /* status */, errno, "could not open file %s",
            PROC_NET_IF_INET6_PATH);
      goto errorout;
    }

    char ifc[IFNAMSIZ], ip[ADDRSTRLEN];
    while (fscanf(if_inet6, "%s %*s %*s %*s %*s %s", ip, ifc) != EOF) {
      char *ifc_p = ifc;
      if (!args.interfaces_specified ||
          lfind(&ifc_p, args.interfaces, &args.num_interfaces, sizeof(char *),
                cmp)) {
        char pretty_ip[ADDRSTRLEN];
        snprintf(pretty_ip, sizeof(pretty_ip),
                 "%.4s:%.4s:%.4s:%.4s:%.4s:%.4s:%.4s:%.4s", ip, ip + 4, ip + 8,
                 ip + 12, ip + 16, ip + 20, ip + 24, ip + 28);
        printf("%s\n", pretty_ip);
      }
    }
  }

  return 0;

errorout:
  perror("something went wrong");
  exit(EXIT_FAILURE);
}
