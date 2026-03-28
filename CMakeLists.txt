find_package(eclipse-paho-mqtt-c REQUIRED)
find_package(PahoMqttCpp REQUIRED)
set(PahoMqttC_LIBRARIES eclipse-paho-mqtt-c::paho-mqtt3as)
set(PahoMqttCpp_LIBRARIES PahoMqttCpp::paho-mqttpp3)

add_library(mqtt_dvcf MODULE mqtt_dvcf.cc)

target_include_directories(mqtt_dvcf PRIVATE
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(mqtt_dvcf
    ${PahoMqttC_LIBRARIES}
    ${PahoMqttCpp_LIBRARIES}
    trunk_recorder_library
    Boost::log
    ssl
    crypto
)

install(TARGETS mqtt_dvcf LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/trunk-recorder)
