SUMMARY = "IoT gateway image: minimal system, iotd daemon and MQTT clients"
LICENSE = "MIT"

# Meme base que core-image-minimal, plus les paquets de la passerelle
IMAGE_INSTALL = " \
    packagegroup-core-boot \
    ${CORE_IMAGE_EXTRA_INSTALL} \
    iotd \
    mosquitto-clients \
"

IMAGE_LINGUAS = " "

inherit core-image

IMAGE_ROOTFS_SIZE ?= "8192"
IMAGE_ROOTFS_EXTRA_SPACE:append = "${@bb.utils.contains("DISTRO_FEATURES", "systemd", " + 4096", "", d)}"
