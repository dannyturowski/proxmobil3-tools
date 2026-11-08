# wt32-r5 application component (Arduino 2.0.17 as an ESP-IDF 4.4.7 component, see ../platformio.ini).
# The portable cores from ../../shared and ST's ULD from ../lib are compiled here directly (PlatformIO's
# LDF is off in this mode).  The defines are wt32-r3's platformio.ini build_flags plus the r4 router ones.
set(SHARED ../../shared)
set(ULD ../lib/vl53l5cx_uld/src)
idf_component_register(
    SRCS main.cpp nori_http.cpp nori_tof.cpp
         ${SHARED}/nori_link/src/nori_dev.c
         ${SHARED}/nori_link/src/nori_input.c
         ${SHARED}/nori_link/src/nori_link.c
         ${SHARED}/nori_link/src/nori_motion.c
         ${SHARED}/nori_sense/src/nori_evlog.c
         ${SHARED}/nori_sense/src/nori_presence.c
         ${SHARED}/nori_sense/src/nori_tofsm.c
         ${SHARED}/nori_router/nori_nat.c
         ${SHARED}/nori_router/nori_lan.c
         ${SHARED}/nori_router/nori_dhcps.c
         ${SHARED}/nori_wifi/nori_wnet.c
         ${SHARED}/nori_wifi/nori_wnsm.c
         ${SHARED}/nori_wifi/nori_wnprov.c
         ${ULD}/platform.c
         ${ULD}/vl53l5cx_api.c
    INCLUDE_DIRS . ${SHARED}/nori_link/src ${SHARED}/nori_sense/src ${SHARED}/nori_router ${SHARED}/nori_wifi ${ULD}
    REQUIRES framework-arduinoespressif32 lwip esp_netif esp_wifi esp_eth app_update nvs_flash driver)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NORI_FW_VERSION="wt32-r5.1.3"
    NORI_BAUD=460800              # host side: nori-linkd --baud 460800 (default)
    NORI_HOSTNAME="WT32-ETH01"     # DHCP (Wi-Fi) and ArduinoOTA name (r5.1.3: no mDNS, NORI_OTA_MDNS=0)
    NORI_SENSOR=1                 # VL53L5CX
    NORI_PIN_SDA=33               # header pin "485_EN"
    NORI_PIN_SCL=32               # header pin "CFG"
    NORI_TOF_HZ=5
    NORI_TOF_INTEG_MS=20
    NORI_TOF_I2C_HZ=400000
    NORI_PIN_BTN1=14              # INPUT_PULLUP, active low
    NORI_PIN_BTN2=4
    NORI_PIN_TOG_FIVE=15           # wt32-r5: three-way track selector, INPUT_PULLUP, contact to GND: track with five
    NORI_PIN_TOG_ONE=2           # wt32-r5: other direction: track with one (a strap pin, see main.cpp)
    ND_DIAG_JOIN=1                # DIAG extras as one token: linkd's nl_parse takes at most 10 args
    ND_PRESENCE=1                 # wt32-r3: EV pon/poff with host resync
    ND_EXT=1                      # wt32-r3: DIAG p=<page>, PRESCFG, SENSOR
    NORI_ETH_ADDR=1
    NORI_ETH_POWER=16
    NORI_ETH_MDC=23
    NORI_ETH_MDIO=18
    NORI_TUNNEL_PORT=2223         # wt32-r4: serial SSH recovery tunnel (r1/r3: 2222)
    NORI_DNAT_PORT=2222           # wt32-r4: Wi-Fi TCP 2222 -> PM3 192.168.77.2:2222
    NORI_LAN_DHCP=1)              # wt32-r4: DHCP server on the private Ethernet
