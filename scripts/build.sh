#!/bin/bash
# Construit l'image Yocto de la passerelle IoT.
# Release Yocto 6.0 "wrynose" (LTS), cible qemux86-64.
# Usage : ./scripts/build.sh [dossier_de_travail]   (par defaut : ~/yocto)
# Option : SSTATE_MIRROR=1 ./scripts/build.sh
#   reutilise les resultats deja compiles par le Yocto Project
#   (cache "sstate" en ligne) : build beaucoup plus court si le
#   reseau est bon.
set -e

YOCTO_BRANCH=wrynose        # branche LTS d'OpenEmbedded-Core et des couches
BITBAKE_BRANCH=2.18         # version de BitBake associee a wrynose
LAYER_DIR=$(cd "$(dirname "$0")/.." && pwd)
WORK_DIR=${1:-$HOME/yocto}

# BitBake refuse de demarrer sans la locale en_US.UTF-8
if ! locale -a | grep -qi '^en_US\.utf-\?8$'; then
    echo "Locale en_US.UTF-8 absente : sudo locale-gen en_US.UTF-8" >&2
    exit 1
fi

# 1. Sources : BitBake et les couches, chacune dans son propre depot
mkdir -p "$WORK_DIR/layers"
cd "$WORK_DIR/layers"

clone() {   # clone <url> <dossier> <branche>, seulement si absent
    [ -d "$2" ] || git clone -b "$3" "$1" "$2"
}
clone https://git.openembedded.org/bitbake           bitbake           "$BITBAKE_BRANCH"
clone https://git.openembedded.org/openembedded-core openembedded-core "$YOCTO_BRANCH"
clone https://git.yoctoproject.org/meta-yocto        meta-yocto        "$YOCTO_BRANCH"
clone https://git.openembedded.org/meta-openembedded meta-openembedded "$YOCTO_BRANCH"

# 2. Environnement de build : PATH, variables, et dossier build/
cd "$WORK_DIR"
source layers/openembedded-core/oe-init-build-env build > /dev/null

# 3. Couches : distribution Poky, meta-oe, meta-python et
#    meta-networking (pour mosquitto), puis cette couche
bitbake-layers add-layer \
    "$WORK_DIR/layers/meta-yocto/meta-poky" \
    "$WORK_DIR/layers/meta-openembedded/meta-oe" \
    "$WORK_DIR/layers/meta-openembedded/meta-python" \
    "$WORK_DIR/layers/meta-openembedded/meta-networking" \
    "$LAYER_DIR"

# 4. Configuration locale, ajoutee une seule fois
if ! grep -q "meta-iot-gateway" conf/local.conf; then
    cat >> conf/local.conf << 'EOF'

# --- meta-iot-gateway ---
DISTRO = "poky"
MACHINE = "qemux86-64"
# systemd au lieu de sysvinit (choix par defaut de Poky)
INIT_MANAGER = "systemd"
# Developpement uniquement : root sans mot de passe sur la console.
# Ne jamais livrer une image de production avec ces options.
EXTRA_IMAGE_FEATURES += "allow-empty-password empty-root-password allow-root-login"
# Supprime les fichiers de travail apres chaque recette : beaucoup moins de disque
INHERIT += "rm_work"
EOF
fi

if [ "${SSTATE_MIRROR:-0}" = "1" ] && ! grep -q "sstate.yoctoproject.org" conf/local.conf; then
    cat >> conf/local.conf << 'EOF'
# Cache sstate en ligne du Yocto Project
BB_HASHSERVE_UPSTREAM = "wss://hashserv.yoctoproject.org/ws"
SSTATE_MIRRORS ?= "file://.* http://sstate.yoctoproject.org/all/PATH;downloadfilename=PATH"
EOF
fi

# 5. Build de l'image
bitbake iot-gateway-image

echo "Image : $WORK_DIR/build/tmp/deploy/images/qemux86-64/"
