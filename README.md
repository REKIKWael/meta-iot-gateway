# Passerelle IoT sous Yocto (meta-iot-gateway)

Projet personnel (novembre 2026) : portage sous **Yocto** du démon `iotd` de mon projet [openwrt-iot-gateway](https://github.com/REKIKWael/openwrt-iot-gateway). Couche Yocto pour la release **6.0 « Wrynose » (LTS)**, cible **qemux86-64**, distribution de référence **Poky** avec **systemd**.

## Ce que fait le projet

- **Couche `meta-iot-gateway`** ajoutée à OpenEmbedded-Core, Poky et meta-openembedded
- **Recette `iotd`** : démon C qui publie l'état réseau de la passerelle en MQTT, sa configuration et son service systemd
- **Image `iot-gateway-image`** : système minimal + `iotd` + clients MQTT
- **bbappend** sur `base-files` : nom d'hôte `iot-gateway`, sans modifier la recette d'origine
- **Résultat** : l'image démarre directement en passerelle et le service publie sans intervention (testé sous QEMU, voir [Tests](#tests))

```
+--------------- Yocto / Poky (QEMU) ---------------+          +---- Hôte (Ubuntu / WSL2) ----+
| systemd -> iotd                                   |   MQTT   |                              |
|   lit /etc/iotd.conf                              | -------> | broker mosquitto             |
|   publie l'état réseau en JSON (libmosquitto)     | TCP 1883 | mosquitto_sub 'gateway/#'    |
|   eth0 en DHCP (réseau slirp de QEMU)             |          | 10.0.2.2                     |
+---------------------------------------------------+          +------------------------------+
```

## De OpenWrt à Yocto : correspondances

| Rôle | OpenWrt | Yocto |
|---|---|---|
| Moteur de build | `make` (issu de Buildroot) | **BitBake** |
| Recettes externes | feeds | **couches** (layers) |
| Choix de la cible et des paquets | `make menuconfig` → `.config` | `local.conf` + recette d'image |
| Recette du paquet | `package/iotd/Makefile` | `iotd_0.1.0.bb` |
| Bibliothèque C | musl | glibc |
| Compilateur croisé | `x86_64-openwrt-linux-musl-gcc` | `x86_64-poky-linux-gcc` |
| Configuration du démon | UCI `/etc/config/iotd` (libuci) | fichier `/etc/iotd.conf` (analyseur en C) |
| Gestion du service | procd (`respawn 3600 5 5` : compte les plantages rapides) | systemd (`Restart=on-failure`, `StartLimitBurst=5` : compte tous les démarrages de l'heure) |
| Journal | `logread` | `journalctl -u iotd` |
| Configuration préservée à la mise à jour | `conffiles` | `CONFFILES` |
| Réseau au premier démarrage | script `uci-defaults` | DHCP filaire déjà fourni par la recette `systemd-conf` |
| Modifier un paquet existant | dossier `files/` de l'image | fichier **bbappend** |
| Démarrage sous QEMU | `qemu-system-x86_64 …` | `runqemu` |

Le cœur du démon (MQTT, JSON, disponibilité, testament, arrêt propre) est inchangé. Seules la lecture de la configuration et l'intégration au système changent.

## Structure du dépôt

```
conf/layer.conf                          déclaration de la couche
recipes-iot/iotd/
├── iotd_0.1.0.bb                        recette du démon
└── files/
    ├── iotd.c                           démon C
    ├── iotd.conf                        configuration par défaut (/etc/iotd.conf)
    └── iotd.service                     service systemd
recipes-core/images/iot-gateway-image.bb recette de l'image
recipes-core/base-files/base-files_%.bbappend   nom d'hôte
scripts/build.sh                         préparation et build complets
tests/run_tests.sh                       tests du démon sur le PC
```

## Construire l'image

Testé sur Ubuntu 24.04 sous WSL2, version prise en charge par Yocto 6.0. Sous WSL2, travailler dans le système de fichiers Linux (`~`), jamais dans `/mnt/c`.

```bash
# Paquets de l'hôte (liste de la documentation Yocto)
sudo apt-get install build-essential chrpath cpio debianutils diffstat file gawk gcc git \
  iputils-ping libacl1 libcrypt-dev locales python3 python3-git python3-jinja2 \
  python3-pexpect python3-pip python3-subunit socat texinfo unzip wget xz-utils zstd
sudo locale-gen en_US.UTF-8                # locale exigée par BitBake

git clone https://github.com/REKIKWael/meta-iot-gateway.git ~/meta-iot-gateway
~/meta-iot-gateway/scripts/build.sh                    # travail dans ~/yocto
# ou, avec le cache de compilation en ligne du Yocto Project (plus rapide) :
sudo apt install python3-websockets        # nécessaire pour ce cache
SSTATE_MIRROR=1 ~/meta-iot-gateway/scripts/build.sh
```

Le script :
1. clone BitBake (branche 2.18), OpenEmbedded-Core, meta-yocto et meta-openembedded (branche `wrynose`) ;
2. prépare l'environnement avec `oe-init-build-env` (crée `build/`) ;
3. ajoute les couches avec `bitbake-layers add-layer` : meta-poky, meta-oe, meta-python, meta-networking (pour mosquitto) et cette couche ;
4. complète `conf/local.conf` : `DISTRO = "poky"`, `MACHINE = "qemux86-64"`, `INIT_MANAGER = "systemd"`, connexion root sans mot de passe (développement uniquement), `rm_work` pour économiser le disque ;
5. lance `bitbake iot-gateway-image`.

Le premier build est long (plusieurs heures sans cache) et demande beaucoup de disque : la documentation Yocto recommande 140 Go libres, `rm_work` réduit fortement l'occupation. Avec `SSTATE_MIRROR=1`, 2 830 tâches sur 2 895 (97 %) ont été récupérées du cache ; seules les 65 restantes, dont `iotd`, l'image et `base-files`, ont été compilées localement.

## Démarrer sous QEMU

```bash
# Terminal 1 : broker sur l'hôte
sudo service mosquitto start
mosquitto_sub -h localhost -t 'gateway/#' -v

# Terminal 2 : image Yocto
cd ~/yocto && source layers/openembedded-core/oe-init-build-env build
runqemu qemux86-64 iot-gateway-image nographic slirp snapshot
# attendre "iot-gateway login:", puis root sans mot de passe ; quitter QEMU : Ctrl+A puis X
```

- `slirp` : réseau utilisateur de QEMU, sans droits root ; l'hôte est joignable à `10.0.2.2`, comme dans le projet OpenWrt.
- `snapshot` : obligatoire ici, car l'image disque est compressée en zstd (`.ext4.zst`) ; les modifications sont écrites dans un fichier temporaire et perdues à l'arrêt.

Sur la cible :

```bash
systemctl status iotd
journalctl -u iotd -f
cat /etc/iotd.conf
```

## Tests

### Sur le PC, sans Yocto

```bash
sudo apt install gcc libmosquitto-dev mosquitto mosquitto-clients
./tests/run_tests.sh
```

Le script compile le démon avec `-Wall -Wextra -Werror` et vérifie : configuration valide, valeurs invalides (`port=abc`, dépassement, `port=99999`, `interval=0`, topic avec joker `#`), commentaires et lignes mal formées, puis, avec un broker local, la publication JSON, la disponibilité `online`, `offline` publié par le démon sur SIGTERM et par le broker (testament) sur `kill -9`.

### Sur l'image, sous QEMU (24/11/2026)

| Test | Commande | Résultat |
|---|---|---|
| Premier démarrage | `systemctl status iotd` | service actif sans intervention ; `online` puis JSON toutes les 10 s sur l'hôte ; 1,4 Mo de mémoire |
| Nom d'hôte | message JSON | `"hostname":"iot-gateway"`, adresse `10.0.2.15` sur `eth0` |
| Plantage | `kill -9 $(pidof iotd)` | `offline` publié par le broker (testament), relance par systemd 5 s plus tard, nouveau PID |
| Arrêt normal | `systemctl stop iotd` | `offline` publié par le démon, code de sortie 0, pas de relance |
| Configuration invalide | `port = 99999`, puis `systemctl start iotd` | `invalid port 99999`, relances toutes les 5 s, puis abandon : « Start request repeated too quickly » ; ensuite `systemctl reset-failed iotd` |
| Changement de configuration | `interval = 3`, puis `systemctl restart iotd` | JSON toutes les 3 s |
| Broker arrêté puis relancé | sur l'hôte : `sudo service mosquitto stop`, puis `start` | `connection was lost`, tentatives toutes les 5 s, reconnexion automatique dès le retour du broker |

Au test de configuration invalide, systemd a abandonné après 3 relances et non 5 : `StartLimitBurst=5` compte tous les démarrages de l'heure, y compris le démarrage au boot et la relance du test de plantage.

## Dépannage

- **`Please make sure locale 'en_US.UTF-8' is available`** : `sudo locale-gen en_US.UTF-8`, puis relancer le script.
- **`.zst images are only supported with snapshot mode`** : ajouter `snapshot` à la commande `runqemu`.
- **`User namespaces are not usable by BitBake`** : restriction AppArmor d'Ubuntu 24.04. Solution temporaire : `sudo sysctl kernel.apparmor_restrict_unprivileged_userns=0`.
- **Avertissement WSLv2 et fichier VHDX** : normal ; le disque virtuel de WSL grossit et peut être compacté plus tard.
- **Mémoire insuffisante** : limiter le parallélisme dans `conf/local.conf` (`BB_NUMBER_THREADS = "4"`, `PARALLEL_MAKE = "-j 4"`) ou augmenter la mémoire de WSL dans `%UserProfile%\.wslconfig`.

## Limites et pistes d'amélioration

- Testé sous QEMU uniquement, pas sur du matériel réel
- Connexion MQTT **sans TLS** ni authentification
- Avec libmosquitto 2.1.2 (celle de l'image), un échec de reconnexion est journalisé « Bad file descriptor » au lieu de « Connection refused » : dans `mosquitto__reconnect` (`lib/connect.c`), `get_address` appelle `getpeername` sur le socket déjà invalide, ce qui écrase `errno` (EBADF) avant le retour. Avec libmosquitto 2.0.18 sur le PC, le message est correct. La reconnexion, elle, fonctionne.
- Au redémarrage, le premier état JSON peut arriver juste avant `online` ; pour un ordre strict, publier le premier état depuis le callback de connexion
- Image de **développement** : root sans mot de passe ; à retirer pour une image de production
- Le service tourne en root (avec durcissement systemd) ; piste : utilisateur dédié avec la classe `useradd`
- Pas de redémarrage automatique quand la configuration change (procd le faisait) ; piste : une unité systemd `.path`
- Couches suivies par branche LTS, pas figées sur un commit
- Le pont MQTT ↔ Matter reste dans le dépôt [openwrt-iot-gateway](https://github.com/REKIKWael/openwrt-iot-gateway)

## Licence

MIT
