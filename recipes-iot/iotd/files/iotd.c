/*
 * iotd - IoT gateway status publisher
 *
 * Version Yocto : la configuration est lue dans un fichier texte
 * "cle = valeur" (/etc/iotd.conf) au lieu d'UCI, et le service est
 * gere par systemd au lieu de procd. Le reste (MQTT, JSON,
 * disponibilite, testament, arret propre) est identique a la
 * version OpenWrt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mosquitto.h>

#define IOTD_DEFAULT_CONFIG "/etc/iotd.conf"

struct iotd_config {
    int  enabled;
    char broker[64];
    int  port;
    char topic[128];
    int  interval;
};

/* Contexte de l'application, transmis aux callbacks libmosquitto */
struct iotd_app {
    struct iotd_config cfg;
    char avail_topic[160];      /* <topic>/availability : online / offline */
};

/* Mis a 0 par le gestionnaire de signal pour demander l'arret */
static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;              /* le strict minimum dans un gestionnaire */
}

/* ------------------------------------------------------------------ */
/* Configuration : fichier texte "cle = valeur"                        */
/* ------------------------------------------------------------------ */

/* Supprime les espaces en debut et en fin de chaine (modifie s) */
static char *trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s))
        s++;

    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';

    return s;
}

/* Conversion stricte : refuse "abc", "12abc" et les depassements.
 * *out n'est modifie qu'en cas de succes. */
static int parse_int(const char *txt, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(txt, &end, 10);
    if (end == txt || *end != '\0' || errno == ERANGE ||
        v < INT_MIN || v > INT_MAX)
        return -1;

    *out = (int)v;
    return 0;
}

/* Copie bornee : refuse une valeur vide ou trop longue
 * plutot que de la tronquer en silence. */
static int set_string(char *out, size_t out_len, const char *val)
{
    size_t n = strlen(val);

    if (n == 0 || n >= out_len)
        return -1;

    memcpy(out, val, n + 1);
    return 0;
}

/* Applique une option. Retourne -1 si la valeur est invalide :
 * la valeur par defaut est alors conservee. */
static int apply_option(struct iotd_config *cfg, const char *key,
                        const char *val)
{
    if (strcmp(key, "broker") == 0)
        return set_string(cfg->broker, sizeof(cfg->broker), val);
    if (strcmp(key, "topic") == 0)
        return set_string(cfg->topic, sizeof(cfg->topic), val);
    if (strcmp(key, "port") == 0)
        return parse_int(val, &cfg->port);
    if (strcmp(key, "interval") == 0)
        return parse_int(val, &cfg->interval);
    if (strcmp(key, "enabled") == 0)
        return parse_int(val, &cfg->enabled);

    fprintf(stderr, "iotd: unknown option '%s' ignored\n", key);
    return 0;
}

static int load_config(const char *path, struct iotd_config *cfg)
{
    char line[256];
    unsigned int lineno = 0;
    FILE *f;

    /* Valeurs par defaut d'abord : une option absente ou invalide
     * garde sa valeur par defaut. */
    cfg->enabled  = 1;
    snprintf(cfg->broker, sizeof(cfg->broker), "%s", "127.0.0.1");
    cfg->port     = 1883;
    snprintf(cfg->topic, sizeof(cfg->topic), "%s", "gateway/status");
    cfg->interval = 10;

    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "iotd: cannot open %s (%s), using defaults\n",
                path, strerror(errno));
    } else {
        while (fgets(line, sizeof(line), f) != NULL) {
            char *key, *val, *eq;

            lineno++;

            /* Ligne trop longue : on la saute entierement */
            if (strchr(line, '\n') == NULL && !feof(f)) {
                int c;

                while ((c = fgetc(f)) != '\n' && c != EOF)
                    ;
                fprintf(stderr, "iotd: %s:%u: line too long, ignored\n",
                        path, lineno);
                continue;
            }

            key = trim(line);
            if (*key == '\0' || *key == '#')
                continue;               /* ligne vide ou commentaire */

            eq = strchr(key, '=');
            if (eq == NULL) {
                fprintf(stderr, "iotd: %s:%u: missing '=', ignored\n",
                        path, lineno);
                continue;
            }

            *eq = '\0';                 /* coupe la ligne en deux */
            key = trim(key);
            val = trim(eq + 1);

            if (apply_option(cfg, key, val) != 0)
                fprintf(stderr,
                        "iotd: %s:%u: invalid value for '%s', default kept\n",
                        path, lineno, key);
        }
        fclose(f);
    }

    if (cfg->port < 1 || cfg->port > 65535) {
        fprintf(stderr, "iotd: invalid port %d\n", cfg->port);
        return -1;
    }

    if (cfg->interval < 1) {
        fprintf(stderr, "iotd: invalid interval %d\n", cfg->interval);
        return -1;
    }

    /* Un topic de publication ne peut pas contenir les jokers + et # */
    if (mosquitto_pub_topic_check(cfg->topic) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "iotd: invalid topic '%s'\n", cfg->topic);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Etat du systeme                                                     */
/* ------------------------------------------------------------------ */

static time_t now_monotonic(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static int read_uptime(double *uptime)
{
    FILE *f = fopen("/proc/uptime", "r");
    int n;

    if (f == NULL)
        return -1;
    n = fscanf(f, "%lf", uptime);
    fclose(f);
    return (n == 1) ? 0 : -1;
}

/* Ajoute du texte formate a la fin de buf, sans jamais deborder.
 * Retourne -1 si le buffer est trop petit. */
static int buf_append(char *buf, size_t len, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*off >= len)
        return -1;

    va_start(ap, fmt);
    n = vsnprintf(buf + *off, len - *off, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= len - *off)
        return -1;              /* resultat tronque */

    *off += (size_t)n;
    return 0;
}

/* Construit l'etat de la passerelle en JSON :
 * {"hostname":"iot-gateway","uptime":1234,"interfaces":[{"name":"eth0","ipv4":"10.0.2.15"}]}
 * Les noms d'hote et d'interface ne contiennent pas de caracteres
 * a echapper en JSON. */
static int build_status_json(char *buf, size_t len)
{
    struct ifaddrs *list, *ifa;
    char host[64];
    char addr[INET_ADDRSTRLEN];
    double uptime = 0.0;
    size_t off = 0;
    bool first = true;
    int rc = 0;

    if (gethostname(host, sizeof(host)) != 0)
        snprintf(host, sizeof(host), "%s", "unknown");
    host[sizeof(host) - 1] = '\0';

    read_uptime(&uptime);

    if (buf_append(buf, len, &off,
                   "{\"hostname\":\"%s\",\"uptime\":%.0f,\"interfaces\":[",
                   host, uptime) != 0)
        return -1;

    if (getifaddrs(&list) == 0) {
        for (ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
            struct sockaddr_in *sin;

            if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;

            sin = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr));

            if (buf_append(buf, len, &off, "%s{\"name\":\"%s\",\"ipv4\":\"%s\"}",
                           first ? "" : ",", ifa->ifa_name, addr) != 0) {
                rc = -1;
                break;
            }
            first = false;
        }
        freeifaddrs(list);      /* liberer la liste allouee par getifaddrs */
    }

    if (rc == 0)
        rc = buf_append(buf, len, &off, "]}");

    return rc;
}

/* ------------------------------------------------------------------ */
/* Callbacks MQTT                                                      */
/* ------------------------------------------------------------------ */

static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    struct iotd_app *app = userdata;

    if (rc != 0) {
        fprintf(stderr, "iotd: connection refused: %s\n",
                mosquitto_connack_string(rc));
        return;
    }

    fprintf(stderr, "iotd: connected to broker\n");

    /* Annonce la disponibilite. Message retenu : un nouvel abonne
     * connait immediatement l'etat de la passerelle. */
    mosquitto_publish(mosq, NULL, app->avail_topic,
                      (int)strlen("online"), "online", 1, true);
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;

    fprintf(stderr, "iotd: disconnected (rc=%d)\n", rc);
}

/* ------------------------------------------------------------------ */
/* Programme principal                                                 */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-c config_file]\n"
                    "  -c  configuration file (default: %s)\n",
            prog, IOTD_DEFAULT_CONFIG);
}

int main(int argc, char *argv[])
{
    const char *config_path = IOTD_DEFAULT_CONFIG;
    struct iotd_app app;
    struct mosquitto *mosq;
    struct sigaction sa;
    time_t next_publish = 0;
    int opt;
    int rc;

    /* -c permet de tester le demon sur le PC avec un autre fichier */
    while ((opt = getopt(argc, argv, "c:h")) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    memset(&app, 0, sizeof(app));

    if (load_config(config_path, &app.cfg) != 0)
        return EXIT_FAILURE;

    snprintf(app.avail_topic, sizeof(app.avail_topic), "%s/availability",
             app.cfg.topic);

    fprintf(stderr, "iotd: enabled=%d broker=%s port=%d topic=%s interval=%d\n",
            app.cfg.enabled, app.cfg.broker, app.cfg.port, app.cfg.topic,
            app.cfg.interval);

    if (!app.cfg.enabled) {
        fprintf(stderr, "iotd: disabled in configuration, exiting\n");
        return EXIT_SUCCESS;
    }

    /* Arret propre sur SIGTERM (systemctl stop, kill) et SIGINT (Ctrl+C) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    mosquitto_lib_init();

    /* &app est transmis aux callbacks (parametre userdata) */
    mosq = mosquitto_new("iotd", true, &app);
    if (mosq == NULL) {
        fprintf(stderr, "iotd: mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return EXIT_FAILURE;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    /* Testament : publie par le broker si la passerelle disparait
     * sans se deconnecter proprement. A definir AVANT la connexion. */
    mosquitto_will_set(mosq, app.avail_topic,
                       (int)strlen("offline"), "offline", 1, true);

    rc = mosquitto_connect(mosq, app.cfg.broker, app.cfg.port, 60);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "iotd: initial connect failed: %s\n",
                mosquitto_strerror(rc));

    while (g_running) {
        rc = mosquitto_loop(mosq, 1000, 1);
        if (!g_running)
            break;

        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "iotd: %s, retrying in 5 s\n",
                    mosquitto_strerror(rc));
            sleep(5);           /* interrompu immediatement par un signal */
            if (!g_running)
                break;

            rc = mosquitto_reconnect(mosq);
            if (rc != MOSQ_ERR_SUCCESS)     /* journaliser la vraie cause */
                fprintf(stderr, "iotd: reconnect failed: %s\n",
                        mosquitto_strerror(rc));
            continue;
        }

        if (now_monotonic() >= next_publish) {
            char payload[1024];

            if (build_status_json(payload, sizeof(payload)) == 0) {
                rc = mosquitto_publish(mosq, NULL, app.cfg.topic,
                                       (int)strlen(payload), payload, 1, false);
                if (rc != MOSQ_ERR_SUCCESS)
                    fprintf(stderr, "iotd: publish failed: %s\n",
                            mosquitto_strerror(rc));
            } else {
                fprintf(stderr, "iotd: status too large for buffer\n");
            }

            next_publish = now_monotonic() + app.cfg.interval;
        }
    }

    /* Arret propre : on annonce "offline" soi-meme, car le broker
     * n'envoie pas le testament lors d'une deconnexion normale. */
    fprintf(stderr, "iotd: stopping\n");
    mosquitto_publish(mosq, NULL, app.avail_topic,
                      (int)strlen("offline"), "offline", 1, true);
    mosquitto_loop(mosq, 500, 1);   /* laisser le temps d'envoyer le message */
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return EXIT_SUCCESS;
}
