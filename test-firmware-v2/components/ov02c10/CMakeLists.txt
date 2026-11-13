# OV02C10 driver from Guition's JC4880P443C_I_W SDK (Apache-2.0, Espressif), vendored because
# upstream esp_cam_sensor does not ship it. Registers itself in esp_cam_sensor's auto-detect table.
if(CONFIG_CAMERA_OV02C10)
    set(srcs "ov02c10.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       PRIV_INCLUDE_DIRS private_include
                       REQUIRES esp_cam_sensor esp_sccb_intf esp_driver_gpio)
if(CONFIG_CAMERA_OV02C10_AUTO_DETECT_MIPI_INTERFACE_SENSOR)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ov02c10_detect")
endif()
