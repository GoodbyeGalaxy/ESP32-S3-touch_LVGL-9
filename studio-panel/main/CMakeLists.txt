idf_component_register(
    SRCS
        "main.cpp"
        "ch422g.cpp"
        "display.cpp"
        "touch.cpp"
        "ui.cpp"
        "screens/home.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_lcd_touch
        driver
        freertos
        log
)
