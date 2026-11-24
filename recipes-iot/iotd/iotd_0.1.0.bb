SUMMARY = "IoT gateway status publisher over MQTT"
DESCRIPTION = "Daemon that reads its configuration file and publishes \
the gateway network status to an MQTT broker."
HOMEPAGE = "https://github.com/REKIKWael/meta-iot-gateway"
SECTION = "net"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Sources locales, cherchees dans le dossier files/ a cote de la recette
SRC_URI = " \
    file://iotd.c \
    file://iotd.conf \
    file://iotd.service \
"
S = "${UNPACKDIR}"

# Dependance de compilation : en-tetes et bibliotheque libmosquitto.
# La dependance d'execution (paquet libmosquitto1) est ajoutee
# automatiquement, car le binaire est lie a libmosquitto.so.1.
DEPENDS = "mosquitto"

inherit systemd

SYSTEMD_SERVICE:${PN} = "iotd.service"
SYSTEMD_AUTO_ENABLE = "enable"

# CC, CFLAGS et LDFLAGS sont fournis par Yocto : compilateur croise,
# sysroot, optimisation et options de durcissement.
do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} -Wall -Wextra -o iotd iotd.c -lmosquitto
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 iotd ${D}${bindir}/iotd

	install -d ${D}${sysconfdir}
	install -m 0600 iotd.conf ${D}${sysconfdir}/iotd.conf

	install -d ${D}${systemd_system_unitdir}
	install -m 0644 iotd.service ${D}${systemd_system_unitdir}/iotd.service
}

# Equivalent de conffiles d'OpenWrt : configuration preservee
# lors d'une mise a jour du paquet
CONFFILES:${PN} = "${sysconfdir}/iotd.conf"
