set(src_dirs ".")
set(include_dirs ".")
set(require esp_driver_i2c)

idf_component_register(SRC_DIRS ${src_dirs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${require})
