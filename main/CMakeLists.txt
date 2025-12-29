idf_component_register(
  SRCS "app_main.c" "display.c"
  INCLUDE_DIRS "."
  REQUIRES driver esp_lcd esp_timer esp_io_expander
)
